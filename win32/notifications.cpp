#include "notifications.h"
#include "winrt_util.h"

#include <windows.data.xml.dom.h>
#include <windows.foundation.h>
#include <windows.ui.notifications.h>
#include <shobjidl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

using ABI::Windows::Data::Xml::Dom::IXmlDocument;
using ABI::Windows::Data::Xml::Dom::IXmlDocumentIO;
using ABI::Windows::UI::Notifications::IToastNotification;
using ABI::Windows::UI::Notifications::IToastNotificationFactory;
using ABI::Windows::UI::Notifications::IToastNotificationManagerStatics;
using ABI::Windows::UI::Notifications::IToastNotifier;
using ABI::Windows::UI::Notifications::NotificationSetting;
using ABI::Windows::UI::Notifications::NotificationSetting_Enabled;

namespace win_notify
{
namespace
{

constexpr size_t kMaxQueuedToasts = 32;

struct State
{
    std::string aumid;
    std::string display_name;
    std::string icon_path;

    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Toast> queue;
    bool quit = false;

    std::atomic<bool> ready{false};
    std::atomic<unsigned int> shown{0};
    std::mutex error_mutex;
    std::string error;

    void fail(const std::string &message)
    {
        {
            std::lock_guard<std::mutex> lock(error_mutex);
            error = message;
        }
        fprintf(stderr, "[crossfade/toast] %s\n", message.c_str());
    }
};

std::mutex g_state_mutex;
std::unique_ptr<State> g_state;

// AppUserModelID registration. Windows identifies toast senders by AUMID;
// for unpackaged apps the display name/icon live under HKCU\Software\Classes
// so the notification settings page and the toast header can label us.
void register_aumid(const std::string &aumid, const std::string &display_name,
                    const std::string &icon_path)
{
    const std::wstring key_path = L"Software\\Classes\\AppUserModelId\\" + cfwin::widen(aumid);
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key_path.c_str(), 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        fprintf(stderr, "[crossfade/toast] could not register AppUserModelId\n");
        return;
    }

    if (!display_name.empty())
    {
        const std::wstring name = cfwin::widen(display_name);
        RegSetValueExW(key, L"DisplayName", 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(name.c_str()),
                       (DWORD)((name.size() + 1) * sizeof(wchar_t)));
    }
    if (!icon_path.empty())
    {
        const std::wstring icon = cfwin::widen(icon_path);
        RegSetValueExW(key, L"IconUri", 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(icon.c_str()),
                       (DWORD)((icon.size() + 1) * sizeof(wchar_t)));
        const std::wstring background = L"FF1F1F1F";
        RegSetValueExW(key, L"IconBackgroundColor", 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(background.c_str()),
                       (DWORD)((background.size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(key);
}

std::string build_toast_xml(const Toast &toast)
{
    std::string xml;
    xml.reserve(512 + toast.title.size() + toast.body.size());
    xml += "<toast duration=\"short\" activationType=\"foreground\">";
    xml += "<visual><binding template=\"ToastGeneric\">";
    xml += "<text id=\"1\">";
    xml += cfwin::xml_escape(toast.title);
    xml += "</text>";
    if (!toast.body.empty())
    {
        xml += "<text id=\"2\">";
        xml += cfwin::xml_escape(toast.body);
        xml += "</text>";
    }

    const std::string image = cfwin::looks_like_icon_name(toast.image) ? std::string()
                                                                        : cfwin::to_uri(toast.image);
    if (!image.empty())
    {
        xml += "<image placement=\"appLogoOverride\" hint-crop=\"circle\" src=\"";
        xml += cfwin::xml_escape(image);
        xml += "\"/>";
    }

    xml += "</binding></visual>";
    if (toast.silent)
        xml += "<audio silent=\"true\"/>";
    xml += "</toast>";
    return xml;
}

class Worker
{
public:
    HRESULT init(const State &state)
    {
        cfwin::HString aumid(state.aumid);
        cfwin::HString aumid_class(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager);
        HRESULT hr = RoGetActivationFactory(aumid_class.get(),
                                            __uuidof(IToastNotificationManagerStatics),
                                            manager.put_void());
        if (FAILED(hr) || !manager)
            return FAILED(hr) ? hr : E_NOINTERFACE;

        hr = manager->CreateToastNotifierWithId(aumid.get(), notifier.put());
        if (FAILED(hr) || !notifier)
            return FAILED(hr) ? hr : E_NOINTERFACE;

        cfwin::HString toast_class(RuntimeClass_Windows_UI_Notifications_ToastNotification);
        hr = RoGetActivationFactory(toast_class.get(), __uuidof(IToastNotificationFactory),
                                    factory.put_void());
        if (FAILED(hr) || !factory)
            return FAILED(hr) ? hr : E_NOINTERFACE;

        NotificationSetting setting = NotificationSetting_Enabled;
        if (SUCCEEDED(notifier->get_Setting(&setting)) && setting != NotificationSetting_Enabled)
        {
            // Notifications disabled for this app/system: toasts will be
            // accepted but silently suppressed by Windows. Report it once so
            // diagnostics explain why nothing appears.
            fprintf(stderr, "[crossfade/toast] notifications are disabled in Windows settings\n");
        }
        return S_OK;
    }

    void show(const Toast &toast, State &state)
    {
        if (!notifier)
            return;

        const std::string xml = build_toast_xml(toast);
        cfwin::HString xml_string(xml);

        cfwin::HString document_class(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument);
        cfwin::ComPtr<IInspectable> inspectable;
        HRESULT hr = RoActivateInstance(document_class.get(), inspectable.put());
        if (FAILED(hr) || !inspectable)
        {
            state.fail("XmlDocument activation failed");
            return;
        }

        cfwin::ComPtr<IXmlDocumentIO> document_io;
        hr = inspectable->QueryInterface(__uuidof(IXmlDocumentIO), document_io.put_void());
        if (FAILED(hr) || !document_io)
        {
            state.fail("IXmlDocumentIO unavailable");
            return;
        }

        hr = document_io->LoadXml(xml_string.get());
        if (FAILED(hr))
        {
            state.fail("toast XML rejected");
            cfwin::log_failure("IXmlDocumentIO::LoadXml", hr);
            return;
        }

        cfwin::ComPtr<IXmlDocument> document;
        hr = inspectable->QueryInterface(__uuidof(IXmlDocument), document.put_void());
        if (FAILED(hr) || !document)
        {
            state.fail("IXmlDocument unavailable");
            return;
        }

        cfwin::ComPtr<IToastNotification> notification;
        hr = factory->CreateToastNotification(document.get(), notification.put());
        if (FAILED(hr) || !notification)
        {
            state.fail("CreateToastNotification failed");
            cfwin::log_failure("IToastNotificationFactory::CreateToastNotification", hr);
            return;
        }

        hr = notifier->Show(notification.get());
        if (FAILED(hr))
        {
            state.fail("toast Show failed");
            cfwin::log_failure("IToastNotifier::Show", hr);
            return;
        }
        state.shown++;
    }
private:
    cfwin::ComPtr<IToastNotificationManagerStatics> manager;
    cfwin::ComPtr<IToastNotifier> notifier;
    cfwin::ComPtr<IToastNotificationFactory> factory;
};

void worker_main(State *state)
{
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr))
    {
        state->fail("RoInitialize failed for notifications");
        return;
    }

    Worker worker;
    hr = worker.init(*state);
    if (FAILED(hr))
    {
        char buf[16];
        state->fail(std::string("toast platform unavailable (") +
                    cfwin::hr_hex(hr, buf, sizeof(buf)) + ")");
    }

    state->ready.store(SUCCEEDED(hr), std::memory_order_release);
    state->cv.notify_all();

    for (;;)
    {
        Toast toast;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [state] { return state->quit || !state->queue.empty(); });
            if (state->quit && state->queue.empty())
                break;
            toast = std::move(state->queue.front());
            state->queue.pop_front();
        }
        if (SUCCEEDED(hr))
            worker.show(toast, *state);
    }

    RoUninitialize();
}

} // namespace

bool init(const std::string &aumid, const std::string &display_name,
          const std::string &icon_path)
{
    if (aumid.empty())
        return false;

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (g_state)
            return true;
    }

    // The explicit process AUMID groups taskbar entries and is what the shell
    // reports to SMTC / notification settings.
    HRESULT aumid_hr = SetCurrentProcessExplicitAppUserModelID(cfwin::widen(aumid).c_str());
    if (FAILED(aumid_hr))
        cfwin::log_failure("SetCurrentProcessExplicitAppUserModelID", aumid_hr);
    register_aumid(aumid, display_name, icon_path);

    auto state = std::make_unique<State>();
    state->aumid = aumid;
    state->display_name = display_name;
    state->icon_path = icon_path;

    State *raw = state.get();
    state->thread = std::thread([raw] { worker_main(raw); });

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_state = std::move(state);
    }

    // Give the worker a brief moment so the first launch's notifications are
    // not queued behind initialization. Never block the UI for long.
    std::unique_lock<std::mutex> lock(raw->mutex);
    raw->cv.wait_for(lock, std::chrono::milliseconds(750),
                     [raw] { return raw->ready.load() || !raw->error.empty(); });
    if (raw->ready.load())
        return true;
    {
        std::lock_guard<std::mutex> error_lock(raw->error_mutex);
        // An empty error means the worker is still starting; keep it alive and
        // let queued toasts drain once it is ready.
        return raw->error.empty();
    }
}

void shutdown()
{
    std::unique_ptr<State> state;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        state = std::move(g_state);
    }
    if (!state)
        return;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->quit = true;
    }
    state->cv.notify_all();
    if (state->thread.joinable())
        state->thread.join();
}

void show(const Toast &toast)
{
    State *state = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        state = g_state.get();
    }
    if (!state || toast.title.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->queue.size() >= kMaxQueuedToasts)
            return;
        state->queue.push_back(toast);
    }
    state->cv.notify_one();
}

bool ready()
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    return g_state && g_state->ready.load();
}

unsigned int shown_count()
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    return g_state ? g_state->shown.load() : 0u;
}

std::string last_error()
{
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (!g_state)
        return "not initialized";
    std::lock_guard<std::mutex> error_lock(g_state->error_mutex);
    return g_state->error;
}

} // namespace win_notify
