#include <assert.h>
#include <string.h>

#ifndef __EMSCRIPTEN__
#  ifdef _WIN32
#    ifndef NOMINMAX
#      define NOMINMAX
#    endif
#    include <windows.h>
#  else
#    include <limits.h>
#    include <stdlib.h>
#    include <unistd.h>
#    include <sys/types.h>
#    ifdef __APPLE__
#      include <mach-o/dyld.h>
#    endif
#  endif
#endif

#include "TracyColor.hpp"
#include "TracyPrint.hpp"
#include "TracyUtility.hpp"
#include "TracyView.hpp"
#include "TracyWorker.hpp"
#include "../Fonts.hpp"

namespace tracy
{

#ifndef __EMSCRIPTEN__
#ifdef _WIN32

static std::wstring Utf8ToWide( const char* str )
{
    const auto len = MultiByteToWideChar( CP_UTF8, 0, str, -1, nullptr, 0 );
    if( len == 0 ) return {};
    std::wstring ret( len, L'\0' );
    MultiByteToWideChar( CP_UTF8, 0, str, -1, &ret[0], len );
    ret.resize( len - 1 );
    return ret;
}

static void AppendQuotedCommandArg( std::wstring& cmd, const std::wstring& arg )
{
    cmd.push_back( L'"' );
    size_t backslashes = 0;
    for( const auto c : arg )
    {
        if( c == L'\\' )
        {
            backslashes++;
        }
        else if( c == L'"' )
        {
            cmd.append( backslashes * 2 + 1, L'\\' );
            cmd.push_back( c );
            backslashes = 0;
        }
        else
        {
            cmd.append( backslashes, L'\\' );
            cmd.push_back( c );
            backslashes = 0;
        }
    }
    cmd.append( backslashes * 2, L'\\' );
    cmd.push_back( L'"' );
}

#else

static std::string GetExecutablePath()
{
#ifdef __APPLE__
    uint32_t sz = PATH_MAX;
    std::vector<char> path( sz );
    if( _NSGetExecutablePath( path.data(), &sz ) != 0 )
    {
        path.resize( sz );
        if( _NSGetExecutablePath( path.data(), &sz ) != 0 ) return {};
    }

    char resolved[PATH_MAX];
    if( realpath( path.data(), resolved ) ) return resolved;
    return path.data();
#else
    char path[PATH_MAX];
    const auto sz = readlink( "/proc/self/exe", path, sizeof( path ) - 1 );
    if( sz <= 0 ) return {};
    path[sz] = '\0';
    return path;
#endif
}

#endif
#endif

bool OpenProfilerInNewWindow( const char* tracePath )
{
#ifdef __EMSCRIPTEN__
    return false;
#elif defined( _WIN32 )
    std::vector<wchar_t> exeBuf( MAX_PATH );
    for(;;)
    {
        const auto sz = GetModuleFileNameW( nullptr, exeBuf.data(), uint32_t( exeBuf.size() ) );
        if( sz == 0 ) return false;
        if( sz < exeBuf.size() - 1 )
        {
            exeBuf.resize( sz );
            break;
        }
        exeBuf.resize( exeBuf.size() * 2 );
    }

    const std::wstring exe( exeBuf.begin(), exeBuf.end() );
    const auto trace = Utf8ToWide( tracePath );
    if( trace.empty() ) return false;

    std::wstring cmd;
    AppendQuotedCommandArg( cmd, exe );
    cmd.push_back( L' ' );
    AppendQuotedCommandArg( cmd, trace );

    STARTUPINFOW si = {};
    si.cb = sizeof( si );
    PROCESS_INFORMATION pi = {};
    const auto ok = CreateProcessW( exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi );
    if( ok )
    {
        CloseHandle( pi.hThread );
        CloseHandle( pi.hProcess );
    }
    return ok != 0;
#else
    const auto exe = GetExecutablePath();
    if( exe.empty() ) return false;

    const auto pid = fork();
    if( pid < 0 ) return false;
    if( pid == 0 )
    {
        setsid();
        execl( exe.c_str(), exe.c_str(), tracePath, static_cast<char*>( nullptr ) );
        _exit( 127 );
    }
    return true;
#endif
}

// Short list based on GetTypes() in TracySourceTokenizer.cpp
constexpr const char* TypesList[] = {
    "bool ", "char ", "double ", "float ", "int ", "long ", "short ",
    "signed ", "unsigned ", "void ", "wchar_t ", "size_t ", "int8_t ",
    "int16_t ", "int32_t ", "int64_t ", "intptr_t ", "uint8_t ", "uint16_t ",
    "uint32_t ", "uint64_t ", "ptrdiff_t ", nullptr
};

const char* ShortenZoneName( ShortenName type, const char* name, ImVec2& tsz, float zsz )
{
    assert( type != ShortenName::Never );
    if( name[0] == '<' || name[0] == '[' ) return name;
    if( type == ShortenName::Always ) zsz = 0;

    static char buf[64*1024];
    char tmp[64*1024];

    auto end = name + strlen( name );
    auto ptr = name;
    auto dst = tmp;
    int cnt = 0;
    for(;;)
    {
        auto start = ptr;
        while( ptr < end && *ptr != '<' ) ptr++;
        memcpy( dst, start, ptr - start + 1 );
        dst += ptr - start + 1;
        if( ptr == end ) break;
        cnt++;
        ptr++;
        while( cnt > 0 )
        {
            if( ptr == end ) break;
            if( *ptr == '<' ) cnt++;
            else if( *ptr == '>' ) cnt--;
            ptr++;
        }
        *dst++ = '>';
    }

    end = dst-1;
    ptr = tmp;
    dst = buf;
    cnt = 0;
    for(;;)
    {
        auto start = ptr;
        while( ptr < end && *ptr != '(' ) ptr++;
        memcpy( dst, start, ptr - start + 1 );
        dst += ptr - start + 1;
        if( ptr == end ) break;
        cnt++;
        ptr++;
        while( cnt > 0 )
        {
            if( ptr == end ) break;
            if( *ptr == '(' ) cnt++;
            else if( *ptr == ')' ) cnt--;
            ptr++;
        }
        *dst++ = ')';
    }

    end = dst-1;
    if( end - buf > 6 && memcmp( end-6, " const", 6 ) == 0 )
    {
        dst[-7] = '\0';
        end -= 6;
    }

    ptr = buf;
    for(;;)
    {
        auto match = TypesList;
        while( *match )
        {
            auto m = *match;
            auto p = ptr;
            while( *m )
            {
                if( *m != *p ) break;
                m++;
                p++;
            }
            if( !*m )
            {
                ptr = p;
                break;
            }
            match++;
        }
        if( !*match ) break;
    }

    tsz = ImGui::CalcTextSize( ptr, end );
    if( type == ShortenName::OnlyNormalize || tsz.x < zsz ) return ptr;

    for(;;)
    {
        auto p = ptr;
        while( p < end && *p != ':' ) p++;
        if( p == end ) return ptr;
        p++;
        while( p < end && *p == ':' ) p++;
        ptr = p;
        tsz = ImGui::CalcTextSize( ptr, end );
        if( tsz.x < zsz ) return ptr;
    }
}

void TooltipNormalizedName( const char* name, const char* normalized )
{
    if( ImGui::IsItemHovered() && normalized != name && strcmp( normalized, name ) != 0 )
    {
        const auto scale = ImGui::GetTextLineHeight() / 15.f;
        if( ImGui::CalcTextSize( name ).x > 1400 * scale )
        {
            ImGui::SetNextWindowSize( ImVec2( 1400 * scale, 0 ) );
            ImGui::BeginTooltip();
            ImGui::TextWrapped( "%s", name );
        }
        else
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted( name );
        }
        ImGui::EndTooltip();
    }
}

uint32_t GetThreadColor( uint64_t thread, int depth, bool dynamic )
{
    if( !dynamic ) return 0xFFCC5555;
    return GetHsvColor( thread, depth );
}

uint32_t GetPlotColor( const PlotData& plot, const Worker& worker )
{
    switch( plot.type )
    {
    case PlotType::User:
        if( plot.color != 0 ) return plot.color | 0xFF000000;
        return GetHsvColor( charutil::hash( worker.GetString( plot.name ) ), -10 );
    case PlotType::Memory:
        return 0xFF2266CC;
    case PlotType::SysTime:
        return 0xFFBAB220;
    case PlotType::Power:
        return 0xFF33CC33;
    default:
        assert( false );
        return 0;
    }
}

const char* FormatPlotValue( double val, PlotValueFormatting format )
{
    static char buf[64];
    switch( format )
    {
    case PlotValueFormatting::Number:
        return RealToString( val );
        break;
    case PlotValueFormatting::Memory:
        return MemSizeToString( val );
        break;
    case PlotValueFormatting::Percentage:
        sprintf( buf, "%.2f%%", val );
        break;
    case PlotValueFormatting::Watt:
        sprintf( buf, "%s W", RealToString( val ) );
        break;
    default:
        assert( false );
        break;
    }
    return buf;
}

std::vector<std::string> SplitLines( const char* data, size_t sz )
{
    std::vector<std::string> ret;
    auto txt = data;
    for(;;)
    {
        auto end = txt;
        while( *end != '\n' && *end != '\r' && end - data < sz ) end++;
        ret.emplace_back( txt, end );
        if( end - data == sz ) break;
        if( *end == '\n' )
        {
            end++;
            if( end - data < sz && *end == '\r' ) end++;
        }
        else if( *end == '\r' )
        {
            end++;
            if( end - data < sz && *end == '\n' ) end++;
        }
        if( end - data == sz ) break;
        txt = end;
    }
    return ret;
}

void PrintLocalStack( const CallstackFrameData* frame, const Worker& worker, const View& view )
{
    for( uint8_t i=0; i<frame->size; i++ )
    {
        ImGui::TextDisabled( "%i.", i+1 );
        ImGui::SameLine();
        const auto symName = worker.GetString( frame->data[i].name );
        const auto normalized = view.GetShortenName() != ShortenName::Never ? ShortenZoneName( ShortenName::OnlyNormalize, symName ) : symName;
        if( worker.IsFrameExternal( frame->data[i].file, frame->imageName ) )
        {
            TextDisabledUnformatted( normalized );
        }
        else
        {
            ImGui::TextUnformatted( normalized );
        }
        ImGui::SameLine();
        ImGui::PushFont( g_fonts.normal, FontSmall );
        ImGui::AlignTextToFramePadding();
        const auto srcline = frame->data[i].line;
        if( srcline != 0 )
        {
            ImGui::TextDisabled( "%s:%i", worker.GetString( frame->data[i].file ), srcline );
        }
        else
        {
            ImGui::TextDisabled( "%s", worker.GetString( frame->data[i].file ) );
        }
        ImGui::PopFont();
    }
}

}
