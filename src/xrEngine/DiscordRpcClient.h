#pragma once

#include <Windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace discord { class Activity; }

namespace quark_discord
{
inline constexpr char application_id[] = "1558958662918078576";
inline constexpr char application_name[] = "Quark Engine";
inline constexpr char community_url[] = "https://discord.com/invite/X7GRYsNNEc";

// Local IPC transport. All methods, including destruction, belong to the
// Discord worker; the game thread only queues a request to clear presence.
class Client
{
public:
    explicit Client(const wchar_t* pipe_prefix = L"\\\\?\\pipe\\discord-ipc-");
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    bool Connect();
    void Pump();
    bool Ready() const { return ready_; }
    bool Submit(const discord::Activity& activity);
    void Clear();
    void Close();
    bool Rejected() const { return rejected_; }
    bool ActivityAccepted() const { return accepted_; }

    static std::string ActivityJson(const discord::Activity& activity);

private:
    bool Transfer(bool write, void* buffer, DWORD size);
    bool Send(std::uint32_t opcode, const std::string& payload);
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
    std::wstring pipe_prefix_;
    std::vector<unsigned char> input_;
    std::string last_activity_;
    ULONGLONG retry_after_ = 0;
    ULONGLONG connected_at_ = 0;
    ULONGLONG last_update_ = 0;
    std::uint64_t nonce_ = 0;
    bool ready_ = false;
    bool rejected_ = false;
    bool accepted_ = false;
};
}
