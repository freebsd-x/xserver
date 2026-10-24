#include "xserver/ipc_socket.h"
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>
#include <cassert>

using namespace fbx::xserver;

int main() {
    std::cout << "Starting Test 3: Isolated IPC Zero-Copy Transfer [C++23 Strict Mode]...\n";

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        std::cerr << "socketpair failed.\n";
        return 1;
    }

    IpcSocket client(UniqueFd{sv[0]});
    IpcSocket server(UniqueFd{sv[1]});

    // Create memfd
    UniqueFd mem_fd(memfd_create("test_memfd", MFD_CLOEXEC));
    if (!mem_fd) {
        std::cerr << "memfd_create failed.\n";
        return 1;
    }

    // Write a byte
    const char* msg = "Z";
    if (write(mem_fd.get(), msg, 1) != 1) {
        std::cerr << "write failed.\n";
        return 1;
    }

    std::cout << "Client sending FD: " << mem_fd.get() << "\n";
    
    // Send over IPC
    auto send_res = client.sendMessage({}, mem_fd.get());
    if (!send_res) {
        std::cerr << "sendMessage failed: " << send_res.error().message() << "\n";
        return 1;
    }

    // Receive over IPC
    auto recv_res = server.receiveMessage();
    if (!recv_res) {
        std::cerr << "receiveMessage failed: " << recv_res.error().message() << "\n";
        return 1;
    }

    auto& [data, received_fd] = recv_res.value();
    
    if (!received_fd) {
        std::cerr << "No FD received!\n";
        return 1;
    }

    std::cout << "Server received FD: " << received_fd.get() << "\n";

    // Read from received FD
    lseek(received_fd.get(), 0, SEEK_SET); // rewind
    char read_buf[2] = {0};
    if (read(received_fd.get(), read_buf, 1) != 1) {
        std::cerr << "read failed.\n";
        return 1;
    }

    std::cout << "Server read from FD: " << read_buf << "\n";

    if (read_buf[0] != 'Z') {
        std::cerr << "Data mismatch!\n";
        return 1;
    }

    std::cout << "Test 3 passed perfectly. SCM_RIGHTS zero-copy FD passing verified.\n";

    return 0;
}
