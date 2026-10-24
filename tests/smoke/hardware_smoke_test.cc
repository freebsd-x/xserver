#include "xserver/unique_fd.h"
#include <gtest/gtest.h>
#include <fcntl.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

namespace fbx::xserver::testing {

class HardwareSmokeTest : public ::testing::Test {};

TEST_F(HardwareSmokeTest, DrmNodesAreAccessible) {
    // Attempt to open the primary KMS node
    UniqueFd card_fd(::open("/dev/dri/card0", O_RDWR | O_CLOEXEC));
    if (!card_fd.isValid()) {
        GTEST_SKIP() << "Hardware skip: Cannot open /dev/dri/card0. Running in headless CI?";
    }

    // Attempt to open the render node
    UniqueFd render_fd(::open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC));
    if (!render_fd.isValid()) {
        GTEST_SKIP() << "Hardware skip: Cannot open /dev/dri/renderD128. Missing GPU drivers?";
    }

    EXPECT_TRUE(card_fd.isValid());
    EXPECT_TRUE(render_fd.isValid());
}

TEST_F(HardwareSmokeTest, VulkanInstanceCreation) {
    VkApplicationInfo app_info{};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "FreeBSD-X Smoke Test";
    app_info.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

    VkInstance instance = VK_NULL_HANDLE;
    VkResult res = vkCreateInstance(&create_info, nullptr, &instance);

    if (res == VK_ERROR_INCOMPATIBLE_DRIVER || res == VK_ERROR_INITIALIZATION_FAILED) {
        GTEST_SKIP() << "Hardware skip: Vulkan ICD not found or incompatible.";
    }

    // If it didn't fail due to missing drivers, we expect success
    ASSERT_EQ(res, VK_SUCCESS);
    EXPECT_NE(instance, VK_NULL_HANDLE);

    vkDestroyInstance(instance, nullptr);
}

} // namespace fbx::xserver::testing