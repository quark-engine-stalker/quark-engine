#include "stdafx.h"
#include "DiscordRpcClient.h"
#include "../3rd party/discord/types.h"
#include <cstring>

namespace quark_discord
{
namespace
{
constexpr DWORD max_payload = 64 * 1024;
constexpr DWORD io_timeout_ms = 50;
constexpr ULONGLONG reconnect_ms = 3000;
constexpr ULONGLONG update_interval_ms = 5000;
struct Header { std::uint32_t opcode, length; };
static_assert(sizeof(Header) == 8, "Discord IPC header is two little-endian u32 values");

void json_string(std::string& out, const char* text)
{
    out += '"';
    // The SDK activity fields are at most 128 bytes. Avoid emitting a partial
    // UTF-8 code point when a game string was truncated by an earlier snprintf.
    for (std::size_t i = 0; text && text[i] && i < 128;)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0x80)
        {
            const unsigned count = c >= 0xc2 && c <= 0xdf ? 2 :
                c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
            bool valid = count && i + count <= 128;
            for (unsigned k = 1; valid && k < count; ++k)
                valid = text[i + k] && (static_cast<unsigned char>(text[i + k]) & 0xc0) == 0x80;
            if (valid && count >= 3)
            {
                const unsigned char next = static_cast<unsigned char>(text[i + 1]);
                valid = !(c == 0xe0 && next < 0xa0) && !(c == 0xed && next >= 0xa0) &&
                    !(c == 0xf0 && next < 0x90) && !(c == 0xf4 && next >= 0x90);
            }
            if (!valid) { ++i; continue; }
            out.append(text + i, count); i += count; continue;
        }
        ++i;
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20)
        {
            constexpr char hex[] = "0123456789abcdef";
            out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15];
        }
        else out += static_cast<char>(c);
    }
    out += '"';
}

void field(std::string& out, const char* key, const char* value)
{
    if (!value || !*value) return;
    out += ','; json_string(out,key); out += ':'; json_string(out,value);
}

// Only dispatch names are needed here. Ignore strings inside nested data and
// escaped quotes; never interpret arbitrary message text as READY or ERROR.
enum class Event { None, Ready, Error, Accepted };
Event event_name(const std::string& json)
{
    int depth = 0; Event event = Event::None; bool activity_reply=false;
    for (std::size_t i = 0; i < json.size(); ++i)
    {
        const char c = json[i];
        if (c == '{' || c == '[') { ++depth; continue; }
        if (c == '}' || c == ']') { if (--depth < 0) return Event::None; continue; }
        if (c != '"') continue;
        const std::size_t start = ++i;
        while (i < json.size() && json[i] != '"')
        { if (json[i] == '\\') ++i; ++i; }
        if (i >= json.size()) return Event::None;
        const bool is_event=json.compare(start,i-start,"evt")==0;
        const bool is_command=json.compare(start,i-start,"cmd")==0;
        if (depth != 1 || (!is_event && !is_command)) continue;
        std::size_t next = i+1;
        while (next < json.size() && (json[next]==' ' || json[next]=='\r' || json[next]=='\n' || json[next]=='\t')) ++next;
        if (next >= json.size() || json[next++] != ':') continue;
        while (next < json.size() && (json[next]==' ' || json[next]=='\r' || json[next]=='\n' || json[next]=='\t')) ++next;
        if (is_event && json.compare(next,7,"\"READY\"") == 0) event = Event::Ready;
        else if (is_event && json.compare(next,7,"\"ERROR\"") == 0) event = Event::Error;
        else if (is_command && json.compare(next,14,"\"SET_ACTIVITY\"") == 0) activity_reply=true;
    }
    if (depth != 0) return Event::None;
    return event==Event::None && activity_reply ? Event::Accepted : event;
}
}

Client::Client(const wchar_t* prefix) : pipe_prefix_(prefix) {}
Client::~Client() { Close(); if (event_) CloseHandle(event_); }

void Client::Close()
{
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE; ready_ = false; rejected_ = false; accepted_=false;
    last_activity_.clear(); last_update_ = 0;
    retry_after_ = GetTickCount64() + reconnect_ms;
}

bool Client::Transfer(bool write, void* buffer, DWORD size)
{
    if (!size) return true;
    ResetEvent(event_); OVERLAPPED operation{}; operation.hEvent=event_;
    DWORD bytes = 0;
    const BOOL result = write ? WriteFile(pipe_,buffer,size,&bytes,&operation) :
        ReadFile(pipe_,buffer,size,&bytes,&operation);
    if (!result)
    {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(event_,io_timeout_ms) != WAIT_OBJECT_0)
        {
            // CancelIoEx is asynchronous. Drain completion before the stack
            // OVERLAPPED or the caller's buffer can leave scope.
            CancelIoEx(pipe_,&operation);
            GetOverlappedResult(pipe_,&operation,&bytes,TRUE);
            return false;
        }
        if (!GetOverlappedResult(pipe_,&operation,&bytes,FALSE)) return false;
    }
    return bytes == size;
}

bool Client::Send(std::uint32_t opcode, const std::string& payload)
{
    if (pipe_ == INVALID_HANDLE_VALUE || payload.size() > max_payload) return false;
    Header header{opcode,static_cast<std::uint32_t>(payload.size())};
    std::vector<unsigned char> packet(sizeof(header)+payload.size());
    std::memcpy(packet.data(),&header,sizeof(header));
    if (!payload.empty()) std::memcpy(packet.data()+sizeof(header),payload.data(),payload.size());
    if (!Transfer(true,packet.data(),static_cast<DWORD>(packet.size()))) { Close(); return false; }
    return true;
}

bool Client::Connect()
{
    if (pipe_ != INVALID_HANDLE_VALUE) return true;
    const ULONGLONG now=GetTickCount64();
    if (now < retry_after_) return false;
    retry_after_=now+reconnect_ms;
    if (!event_) event_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!event_) return false;
    for (unsigned index=0; index<10; ++index)
    {
        const std::wstring path=pipe_prefix_+std::to_wstring(index);
        pipe_=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) break;
    }
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    connected_at_=now;
    return Send(0,std::string("{\"v\":1,\"client_id\":\"")+application_id+"\"}");
}

void Client::Pump()
{
    if (pipe_ == INVALID_HANDLE_VALUE) return;
    if (!ready_ && GetTickCount64()-connected_at_ > 10000) { Close(); return; }
    for (unsigned iteration=0; iteration<16; ++iteration)
    {
        Header header{};DWORD available=0,peeked=0;
        if (!PeekNamedPipe(pipe_,&header,sizeof(header),&peeked,&available,nullptr)) { Close(); return; }
        if (peeked < sizeof(header)) return;
        if (header.length > max_payload || header.opcode > 4) { Close(); return; }
        const DWORD bytes=sizeof(header)+header.length;
        if (available < bytes) return;
        input_.resize(bytes);
        if (!Transfer(false,input_.data(),bytes)) { Close(); return; }
        std::string payload(reinterpret_cast<const char*>(input_.data()+sizeof(header)),header.length);
        if (header.opcode == 2) { Close(); return; }
        if (header.opcode == 3) { if (!Send(4,payload)) return; }
        else if (header.opcode == 1)
        {
            const Event event=event_name(payload);
            if (event == Event::Ready) { ready_=true; rejected_=false; }
            else if (event == Event::Error) { rejected_=true; accepted_=false; last_activity_.clear(); }
            else if (event == Event::Accepted) { accepted_=true; rejected_=false; }
        }
    }
}

std::string Client::ActivityJson(const discord::Activity& activity)
{
    std::string out="{\"type\":0,\"name\":";json_string(out,application_name);
    field(out,"state",activity.GetState());field(out,"details",activity.GetDetails());
    const auto start=activity.GetTimestamps().GetStart();
    if (start > 0) out+=",\"timestamps\":{\"start\":"+std::to_string(start)+"}";
    const auto& assets=activity.GetAssets();
    if (*assets.GetLargeImage() || *assets.GetSmallImage())
    {
        out+=",\"assets\":{\"large_image\":";json_string(out,assets.GetLargeImage());
        field(out,"large_text",assets.GetLargeText());field(out,"small_image",assets.GetSmallImage());field(out,"small_text",assets.GetSmallText());
        out+='}';
    }
    out+=",\"buttons\":[{\"label\":\"Discord\",\"url\":";
    json_string(out,community_url);out+="}]}";return out;
}

bool Client::Submit(const discord::Activity& activity)
{
    if (!ready_) return false;
    std::string json=ActivityJson(activity);
    if (json==last_activity_) return true;
    const auto now=GetTickCount64();
    if (last_update_ && now-last_update_ < update_interval_ms) return false;
    const std::string payload="{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"+
        std::to_string(GetCurrentProcessId())+",\"activity\":"+json+"},\"nonce\":\""+std::to_string(++nonce_)+"\"}";
    if (!Send(1,payload)) return false;
    last_activity_=std::move(json);last_update_=now;accepted_=false;return true;
}

void Client::Clear()
{
    if (ready_) Send(1,"{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"+
        std::to_string(GetCurrentProcessId())+",\"activity\":null},\"nonce\":\""+std::to_string(++nonce_)+"\"}");
    last_activity_.clear();last_update_=0;accepted_=false;
}
}
