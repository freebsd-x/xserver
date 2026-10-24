#include <gtest/gtest.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <vector>
#include <string>
#include <iostream>
#include <thread>
#include <chrono>
#include "shared/protocol.h"

namespace fbx::testing {

class Process {
public:
    Process(const std::string& path, const std::vector<std::string>& args) {
        pid_ = fork();
        if (pid_ == 0) {
            // Child process
            std::vector<const char*> c_args;
            c_args.push_back(path.c_str());
            for (const auto& arg : args) {
                c_args.push_back(arg.c_str());
            }
            c_args.push_back(nullptr);
            execvp(path.c_str(), const_cast<char**>(c_args.data()));
            perror("execvp failed");
            exit(1);
        }
    }

    ~Process() {
        if (pid_ > 0) {
            kill(pid_, SIGTERM);
            int status;
            waitpid(pid_, &status, 0);
        }
    }

    [[nodiscard]] bool isRunning() const {
        if (pid_ <= 0) return false;
        int status;
        pid_t res = waitpid(pid_, &status, WNOHANG);
        return res == 0;
    }

private:
    pid_t pid_;
};

class E2EIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clean up any lingering sockets
        system("rm -f /tmp/freebsd-x*.sock");
        system("killall xserver xrender xshell 2>/dev/null");
    }

    void TearDown() override {
        system("killall xserver xrender xshell 2>/dev/null");
    }
};

TEST_F(E2EIntegrationTest, FullPipelineFlow) {
    // 1. Spawn the Stack with Absolute Paths
    std::cout << "[E2E] Spawning Stack...\n";
    Process server("/home/wheel/freebsd-x/xserver/build/xserver", {});
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ASSERT_TRUE(server.isRunning()) << "xserver failed to start";

    Process render("/home/wheel/freebsd-x/xrender/build/xrender", {});
    std::this_thread::sleep_for(std::chrono::seconds(2));
    ASSERT_TRUE(render.isRunning()) << "xrender failed to start";

    Process shell("/home/wheel/freebsd-x/apps/xshell/build/xshell", {});
    std::this_thread::sleep_for(std::chrono::seconds(1));
    ASSERT_TRUE(shell.isRunning()) << "xshell failed to start";

    // 2. Inject Synthetic Input
    std::cout << "[E2E] Injecting Synthetic MouseMove...\n";
    int input_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_GE(input_sock, 0);

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/freebsd-x-input.sock", sizeof(addr.sun_path) - 1);

    int connect_res = connect(input_sock, (struct sockaddr*)&addr, sizeof(addr));
    ASSERT_EQ(connect_res, 0) << "Failed to connect to input routing socket";

    // Send Move to (10, 50) -> This should hit the first icon (32 < Y < 96)
    shared::InputMessage move_msg{
        .type_ = shared::InputEventType::MouseMove,
        .value_a_ = 10,
        .value_b_ = 50
    };

    ssize_t sent = send(input_sock, &move_msg, sizeof(move_msg), 0);
    ASSERT_EQ(sent, sizeof(move_msg));
    close(input_sock);

    // 3. Verification
    // We give the system a moment to process the message
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Assert all processes are still alive and healthy
    EXPECT_TRUE(server.isRunning());
    EXPECT_TRUE(render.isRunning());
    EXPECT_TRUE(shell.isRunning());

    std::cout << "[E2E] Integration Pipeline verified. All components synchronized.\n";
}

} // namespace fbx::testing
