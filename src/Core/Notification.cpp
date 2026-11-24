#include <Core/Notification.h>
#include <ctime>
#include <format>

std::string Notification::timeText() const noexcept
{
    using namespace std::chrono;
    const auto elapsed { duration_cast<minutes>(system_clock::now() - time).count() };

    if (elapsed < 1)
        return "now";

    if (elapsed < 60)
        return std::format("{}m ago", elapsed);

    if (elapsed < 24 * 60)
        return std::format("{}h ago", elapsed / 60);

    const time_t t { system_clock::to_time_t(time) };
    tm local;
    localtime_r(&t, &local);
    char text[32];
    strftime(text, sizeof(text), "%b %e", &local);
    return text;
}
