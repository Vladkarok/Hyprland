#include "../../hyprctlCompat.hpp"
#include "../shared.hpp"
#include "tests.hpp"
#include "build.hpp"

#include <hyprutils/os/FileDescriptor.hpp>
#include <hyprutils/os/Process.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

#include <optional>
#include <format>
#include <sys/poll.h>
#include <csignal>
#include <thread>

using namespace Hyprutils::OS;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Utils;

#define SP CSharedPointer

namespace {

    class CClient {
        SP<CProcess>           proc;
        std::array<char, 1024> readBuf;
        CFileDescriptor        readFd, writeFd;
        struct pollfd          fds;

      public:
        CClient();
        ~CClient();
        uint32_t getLockedMods();
        uint32_t getGroup();
        uint32_t getDepressedMods();
        pid_t    pid();

      private:
        uint32_t query(const std::string& what);
    };
}

CClient::CClient() {
    Tests::killAllWindows();
    this->proc = makeShared<CProcess>(std::format("{}/keyboard-modifiers", binaryDir), std::vector<std::string>{});

    this->proc->addEnv("WAYLAND_DISPLAY", WLDISPLAY);

    int pipeFds1[2], pipeFds2[2];
    if (pipe(pipeFds1) != 0 || pipe(pipeFds2) != 0) {
        NLog::log("{}Unable to open pipe to client", Colors::RED);
        throw std::exception();
    }

    this->writeFd = CFileDescriptor(pipeFds1[1]);
    this->proc->setStdinFD(pipeFds1[0]);

    this->readFd = CFileDescriptor(pipeFds2[0]);
    this->proc->setStdoutFD(pipeFds2[1]);

    const int COUNT_BEFORE = Tests::windowCount();
    this->proc->runAsync();

    close(pipeFds1[0]);
    close(pipeFds2[1]);

    this->fds = {.fd = this->readFd.get(), .events = POLLIN};
    if (poll(&this->fds, 1, 1000) != 1 || !(this->fds.revents & POLLIN)) {
        NLog::log("{}keyboard-modifiers client failed poll", Colors::RED);
        throw std::exception();
    }

    this->readBuf.fill(0);
    if (read(this->readFd.get(), this->readBuf.data(), this->readBuf.size() - 1) == -1) {
        NLog::log("{}keyboard-modifiers client read failed", Colors::RED);
        throw std::exception();
    }

    std::string ret = std::string{this->readBuf.data()};
    if (ret.find("started") == std::string::npos) {
        NLog::log("{}Failed to start keyboard-modifiers client, read {}", Colors::RED, ret);
        throw std::exception();
    }

    int counter = 0;
    while (Tests::processAlive(this->proc->pid()) && Tests::windowCount() == COUNT_BEFORE) {
        counter++;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        if (counter > 50) {
            NLog::log("{}keyboard-modifiers client took too long to open", Colors::RED);
            throw std::exception();
        }
    }

    if (!Tests::processAlive(this->proc->pid())) {
        NLog::log("{}keyboard-modifiers client not alive", Colors::RED);
        throw std::exception();
    }

    if (getFromSocket(std::format("/dispatch hl.dsp.focus({{ window = 'pid:{}' }})", this->proc->pid())) != "ok") {
        NLog::log("{}Failed to focus keyboard-modifiers client", Colors::RED);
        throw std::exception();
    }

    NLog::log("{}Started keyboard-modifiers client", Colors::YELLOW);
}

CClient::~CClient() {
    getFromSocket("/eval hl.plugin.test.set_mods(0, 0, 0, 0, 0)");
    getFromSocket("/eval hl.plugin.test.set_mods(1, 0, 0, 0, 0)");

    std::string cmd = "exit\n";
    write(this->writeFd.get(), cmd.c_str(), cmd.length());

    kill(this->proc->pid(), SIGKILL);
    this->proc.reset();
}

uint32_t CClient::query(const std::string& what) {
    std::string cmd = what + "\n";
    if (sc<size_t>(write(this->writeFd.get(), cmd.c_str(), cmd.length())) != cmd.length())
        return UINT32_MAX;

    if (poll(&this->fds, 1, 1500) != 1 || !(this->fds.revents & POLLIN))
        return UINT32_MAX;
    ssize_t bytesRead = read(this->fds.fd, this->readBuf.data(), 1023);
    if (bytesRead <= 0)
        return UINT32_MAX;

    this->readBuf[bytesRead] = 0;
    std::string received     = std::string{this->readBuf.data()};
    received.pop_back();

    try {
        return std::stoul(received);
    } catch (...) { return UINT32_MAX; }
}

uint32_t CClient::getLockedMods() {
    return query("get");
}

uint32_t CClient::getGroup() {
    return query("group");
}

uint32_t CClient::getDepressedMods() {
    return query("depressed");
}

pid_t CClient::pid() {
    return this->proc->pid();
}

TEST_CASE(keyboardModifiersMergedOnFocus) {
    NLog::log("{}Testing keyboard modifiers merged on focus", Colors::GREEN);

    std::optional<CClient> client;

    try {
        client.emplace();
    } catch (...) { FAIL_TEST("Couldn't start the client"); }

    EXPECT(client->getLockedMods(), 0u);

    OK(getFromSocket("/eval hl.plugin.test.nullfocus()"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    OK(getFromSocket("/eval hl.plugin.test.set_mods(0, 0, 0, 2, 0)"));
    OK(getFromSocket("/eval hl.plugin.test.set_mods(1, 0, 0, 16, 0)"));

    if (getFromSocket(std::format("/dispatch hl.dsp.focus({{ window = 'pid:{}' }})", client->pid())) != "ok") {
        FAIL_TEST("Failed to refocus keyboard-modifiers client");
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    const uint32_t locked = client->getLockedMods();
    NLog::log("{}Client reports locked mods: {}", Colors::BLUE, locked);
    EXPECT(locked, 18u);
}

// The layout-group tests need two layouts. Keyboards are addressed by plugin slot: 0 and 1 are the plugin's
// two physical test keyboards, 2.. are created per test. KEY_A is 30.
static constexpr uint32_t KEY_A = 30;

static bool               refocus(pid_t pid) {
    if (getFromSocket("/eval hl.plugin.test.nullfocus()") != "ok")
        return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool OK = getFromSocket(std::format("/dispatch hl.dsp.focus({{ window = 'pid:{}' }})", pid)) == "ok";
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return OK;
}

static void settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// Restores what the layout tests change, whatever way the test ends.
static CScopeGuard layoutTestCleanup() {
    return CScopeGuard([] {
        for (int slot = 2; slot < 6; ++slot)
            getFromSocket(std::format("/eval hl.plugin.test.kb_destroy({})", slot));
        getFromSocket("/eval hl.device({ name = 'test-keyboard-1', enabled = true, tags = 'test-tag' })");
        getFromSocket("/eval hl.plugin.test.kb_enable(0, true)");
        getFromSocket("/eval hl.plugin.test.kb_enable(1, true)");
        getFromSocket("/eval hl.config({ input = { kb_layout = 'us', numlock_by_default = false } })");
    });
}

// A virtual keyboard appearing (an input method's, an on-screen keyboard's) must not take over the seat:
// the focused client keeps the layout group of the keyboard in use, and that keyboard stays "main".
TEST_CASE(keyboardGroupKeptWhenVirtualKeyboardAppears) {
    NLog::log("{}Testing the seat keyboard survives a virtual keyboard appearing", Colors::GREEN);

    const auto             CLEANUP = layoutTestCleanup();
    std::optional<CClient> client;
    try {
        client.emplace();
    } catch (...) { FAIL_TEST("Couldn't start the client"); }

    OK(getFromSocket("/eval hl.config({ input = { kb_layout = 'us,ua' } })"));
    OK(getFromSocket("/eval hl.plugin.test.kb_mods(0, 0, 0, 0, 1)"));
    OK(getFromSocket(std::format("/eval hl.plugin.test.kb_key(0, {})", KEY_A)));
    settle();
    EXPECT(client->getGroup(), 1u);

    OK(getFromSocket("/eval hl.plugin.test.kb_create(2, true)"));
    OK(getFromSocket("/eval hl.plugin.test.expect_seat_keyboard(0)"));
    settle();
    EXPECT(client->getGroup(), 1u);

    if (!refocus(client->pid()))
        FAIL_TEST("Failed to refocus the client");
    EXPECT(client->getGroup(), 1u);
}

// The same with numlock_by_default: configuring the new keyboard sets NumLock, and that configuration-made
// modifier change must not promote it either.
TEST_CASE(keyboardGroupKeptWhenVirtualKeyboardAppearsWithNumlockByDefault) {
    NLog::log("{}Testing a configured virtual keyboard does not take over the seat", Colors::GREEN);

    const auto             CLEANUP = layoutTestCleanup();
    std::optional<CClient> client;
    try {
        client.emplace();
    } catch (...) { FAIL_TEST("Couldn't start the client"); }

    OK(getFromSocket("/eval hl.config({ input = { kb_layout = 'us,ua', numlock_by_default = true } })"));
    OK(getFromSocket("/eval hl.plugin.test.kb_mods(0, 0, 0, 0, 1)"));
    OK(getFromSocket(std::format("/eval hl.plugin.test.kb_key(0, {})", KEY_A)));

    OK(getFromSocket("/eval hl.plugin.test.kb_create(2, true)"));
    OK(getFromSocket("/eval hl.plugin.test.expect_seat_keyboard(0)"));

    if (!refocus(client->pid()))
        FAIL_TEST("Failed to refocus the client");
    EXPECT(client->getGroup(), 1u);
}

// When the seat keyboard goes away, the seat falls back to the keyboard a key was last pressed on, not to the
// last-added one, and the focused client is told that keyboard's group right away.
TEST_CASE(keyboardSeatFallsBackToLastTypedKeyboard) {
    NLog::log("{}Testing the seat falls back to the last typed-on keyboard", Colors::GREEN);

    const auto             CLEANUP = layoutTestCleanup();
    std::optional<CClient> client;
    try {
        client.emplace();
    } catch (...) { FAIL_TEST("Couldn't start the client"); }

    OK(getFromSocket("/eval hl.config({ input = { kb_layout = 'us,ua' } })"));
    OK(getFromSocket("/eval hl.plugin.test.kb_mods(0, 0, 0, 0, 1)"));
    // an idle physical keyboard on group 0, added last
    OK(getFromSocket("/eval hl.plugin.test.kb_create(3, false)"));
    OK(getFromSocket(std::format("/eval hl.plugin.test.kb_key(0, {})", KEY_A)));
    // a virtual keyboard types, takes over, and goes away
    OK(getFromSocket("/eval hl.plugin.test.kb_create(2, true)"));
    OK(getFromSocket(std::format("/eval hl.plugin.test.kb_key(2, {})", KEY_A)));
    OK(getFromSocket("/eval hl.plugin.test.expect_seat_keyboard(2)"));
    OK(getFromSocket("/eval hl.plugin.test.kb_destroy(2)"));

    OK(getFromSocket("/eval hl.plugin.test.expect_seat_keyboard(0)"));
    settle();
    EXPECT(client->getGroup(), 1u);

    if (!refocus(client->pid()))
        FAIL_TEST("Failed to refocus the client");
    EXPECT(client->getGroup(), 1u);
}

// The fallback skips a disabled keyboard, even the one typed on last, while an enabled one is available.
TEST_CASE(keyboardSeatFallbackSkipsDisabledKeyboard) {
    NLog::log("{}Testing the seat fallback skips a disabled keyboard", Colors::GREEN);

    const auto             CLEANUP = layoutTestCleanup();
    std::optional<CClient> client;
    try {
        client.emplace();
    } catch (...) { FAIL_TEST("Couldn't start the client"); }

    OK(getFromSocket(std::format("/eval hl.plugin.test.kb_key(0, {})", KEY_A)));
    OK(getFromSocket(std::format("/eval hl.plugin.test.kb_key(1, {})", KEY_A)));
    OK(getFromSocket("/eval hl.plugin.test.kb_enable(1, false)"));
    OK(getFromSocket("/eval hl.plugin.test.kb_create(2, true)"));
    OK(getFromSocket(std::format("/eval hl.plugin.test.kb_key(2, {})", KEY_A)));
    OK(getFromSocket("/eval hl.plugin.test.kb_destroy(2)"));

    OK(getFromSocket("/eval hl.plugin.test.expect_seat_keyboard(0)"));
}

// Switching the seat keyboard between keyboards with identical keymaps still tells the client the new
// keyboard's group and locks, as a modifier event from it would.
TEST_CASE(keyboardModifiersFollowSeatKeyboardChange) {
    NLog::log("{}Testing the client follows a seat keyboard change", Colors::GREEN);

    const auto             CLEANUP = layoutTestCleanup();
    std::optional<CClient> client;
    try {
        client.emplace();
    } catch (...) { FAIL_TEST("Couldn't start the client"); }

    OK(getFromSocket("/eval hl.config({ input = { kb_layout = 'us,ua' } })"));
    OK(getFromSocket("/eval hl.plugin.test.kb_mods(0, 0, 0, 2, 1)"));  // keyboard 0: Caps Lock, group 1
    OK(getFromSocket("/eval hl.plugin.test.kb_mods(1, 0, 0, 16, 0)")); // keyboard 1: NumLock, group 0 (now the seat keyboard)
    settle();
    EXPECT(client->getGroup(), 0u);

    OK(getFromSocket(std::format("/eval hl.plugin.test.kb_key(0, {})", KEY_A)));
    OK(getFromSocket("/eval hl.plugin.test.expect_seat_keyboard(0)"));
    settle();
    EXPECT(client->getGroup(), 1u);
    EXPECT(client->getLockedMods(), 2u);
}

// Reloading the configuration (numlock_by_default re-applied to every keyboard) does not move the seat to
// whichever keyboard the configuration changed last. Keyboard 1 holds Shift, so configuring it is a real
// modifier change for it.
TEST_CASE(keyboardSeatKeptOnConfigReload) {
    NLog::log("{}Testing a config reload keeps the seat keyboard", Colors::GREEN);

    const auto             CLEANUP = layoutTestCleanup();
    std::optional<CClient> client;
    try {
        client.emplace();
    } catch (...) { FAIL_TEST("Couldn't start the client"); }

    OK(getFromSocket("/eval hl.config({ input = { numlock_by_default = false } })"));
    OK(getFromSocket("/eval hl.plugin.test.kb_mods(1, 1, 0, 0, 0)"));
    OK(getFromSocket(std::format("/eval hl.plugin.test.kb_key(0, {})", KEY_A)));
    OK(getFromSocket("/eval hl.plugin.test.expect_seat_keyboard(0)"));
    settle();
    EXPECT(client->getDepressedMods(), 1u); // the shared Shift reached the client
    OK(getFromSocket("/eval hl.config({ input = { numlock_by_default = true } })"));
    OK(getFromSocket("/eval hl.plugin.test.expect_seat_keyboard(0)"));
    // configuring keyboard 1 released its Shift: the client must not keep it
    settle();
    EXPECT(client->getDepressedMods(), 0u);
}

// Configuration keeps the seat keyboard only while it is usable: with the seat keyboard disabled, a keyboard
// whose configuration releases a shared Shift is followed, as before, and the client drops the Shift.
TEST_CASE(keyboardConfigReleaseReachesClientWhenSeatKeyboardDisabled) {
    NLog::log("{}Testing configuration with a disabled seat keyboard still reaches the client", Colors::GREEN);

    const auto             CLEANUP = layoutTestCleanup();
    std::optional<CClient> client;
    try {
        client.emplace();
    } catch (...) { FAIL_TEST("Couldn't start the client"); }

    OK(getFromSocket("/eval hl.config({ input = { numlock_by_default = false } })"));
    OK(getFromSocket("/eval hl.plugin.test.kb_mods(1, 1, 0, 0, 0)")); // keyboard 1 holds Shift
    OK(getFromSocket(std::format("/eval hl.plugin.test.kb_key(0, {})", KEY_A)));
    OK(getFromSocket("/eval hl.plugin.test.expect_seat_keyboard(0)"));
    settle();
    EXPECT(client->getDepressedMods(), 1u); // the shared Shift reached the client
    // disabled through its device config, so the reload below keeps it disabled
    OK(getFromSocket("/eval hl.device({ name = 'test-keyboard-1', enabled = false, tags = 'test-tag' })"));
    OK(getFromSocket("/eval hl.config({ input = { numlock_by_default = true } })"));
    OK(getFromSocket("/eval hl.plugin.test.expect_seat_keyboard(1)"));
    settle();
    EXPECT(client->getDepressedMods(), 0u);
}

// Removing a keyboard that is not the seat keyboard leaves the seat keyboard alone.
TEST_CASE(keyboardSeatKeptWhenOtherKeyboardRemoved) {
    NLog::log("{}Testing removing another keyboard keeps the seat keyboard", Colors::GREEN);

    const auto             CLEANUP = layoutTestCleanup();
    std::optional<CClient> client;
    try {
        client.emplace();
    } catch (...) { FAIL_TEST("Couldn't start the client"); }

    OK(getFromSocket("/eval hl.plugin.test.kb_create(3, false)"));
    OK(getFromSocket(std::format("/eval hl.plugin.test.kb_key(0, {})", KEY_A)));
    OK(getFromSocket("/eval hl.plugin.test.expect_seat_keyboard(0)"));
    OK(getFromSocket("/eval hl.plugin.test.kb_destroy(3)"));
    OK(getFromSocket("/eval hl.plugin.test.expect_seat_keyboard(0)"));
}

// Releasing a lock on the seat keyboard reaches the client even while another keyboard sharing state still
// holds it: ordinary modifier events carry the seat keyboard's own locks.
TEST_CASE(keyboardLockReleaseOnOneKeyboardReachesClient) {
    NLog::log("{}Testing a lock released on one keyboard reaches the client", Colors::GREEN);

    const auto             CLEANUP = layoutTestCleanup();
    std::optional<CClient> client;
    try {
        client.emplace();
    } catch (...) { FAIL_TEST("Couldn't start the client"); }

    OK(getFromSocket("/eval hl.plugin.test.kb_mods(1, 0, 0, 16, 0)")); // keyboard 1: NumLock
    OK(getFromSocket("/eval hl.plugin.test.kb_mods(0, 0, 0, 16, 0)")); // keyboard 0: NumLock, the seat keyboard
    OK(getFromSocket("/eval hl.plugin.test.expect_seat_keyboard(0)"));
    settle();
    EXPECT(client->getLockedMods(), 16u);

    OK(getFromSocket("/eval hl.plugin.test.kb_mods(0, 0, 0, 0, 0)")); // NumLock off on keyboard 0
    settle();
    EXPECT(client->getLockedMods(), 0u);
}
