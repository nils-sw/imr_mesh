#include "imr/imr.h"
#include "imr/util.h"

#include <cmath>
#include "nasl/nasl.h"
#include "nasl/nasl_mat.h"

#include "../common/camera.h"

using namespace nasl;

struct Tri { vec3 v0, v1, v2; vec3 color; };

struct {
    VkDeviceAddress vertex_buffer;
    mat4 matrix;
    VkDeviceAddress face_buffer;
    VkDeviceAddress position_buffer;
} push_constants_batched;

Camera camera;
CameraFreelookState camera_state = {
    .fly_speed = 1.0f,
    .mouse_sensitivity = 1,
};
CameraInput camera_input;

void camera_update(GLFWwindow*, CameraInput* input);

bool reload_shaders = false;

#define INSTANCES_COUNT 1024

struct Shaders {
    std::vector<std::string> files = { "meshshader.mesh.spv", "meshshader.task.spv", "meshshader.frag.spv" };

    std::vector<std::unique_ptr<imr::ShaderModule>> modules;
    std::vector<std::unique_ptr<imr::ShaderEntryPoint>> entry_points;
    std::unique_ptr<imr::GraphicsPipeline> pipeline;

    Shaders(imr::Device& d, imr::Swapchain& swapchain) {
        imr::GraphicsPipeline::RenderTargetsState rts;
        rts.color.push_back((imr::GraphicsPipeline::RenderTarget) {
            .format = swapchain.format(),
            .blending = {
                .blendEnable = false,
                .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT
            }
        });
        imr::GraphicsPipeline::RenderTarget depth = {
            .format = VK_FORMAT_D32_SFLOAT
        };
        rts.depth = depth;

        imr::GraphicsPipeline::StateBuilder stateBuilder = {
            .vertexInputState = imr::GraphicsPipeline::no_vertex_input(),
            .inputAssemblyState = imr::GraphicsPipeline::simple_triangle_input_assembly(),
            .viewportState = imr::GraphicsPipeline::one_dynamically_sized_viewport(),
            .rasterizationState = imr::GraphicsPipeline::solid_filled_polygons(),
            .multisampleState = imr::GraphicsPipeline::one_spp(),
            .depthStencilState = imr::GraphicsPipeline::simple_depth_testing(),
        };

        std::vector<imr::ShaderEntryPoint*> entry_point_ptrs;
        for (auto filename : files) {
            VkShaderStageFlagBits stage;
            if (filename.ends_with("mesh.spv"))
                stage = VK_SHADER_STAGE_MESH_BIT_EXT;
            else if (filename.ends_with("task.spv"))
                stage = VK_SHADER_STAGE_TASK_BIT_EXT;
            else if (filename.ends_with("frag.spv"))
                stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            else
                throw std::runtime_error("Unknown suffix");
            modules.push_back(std::make_unique<imr::ShaderModule>(d, std::move(filename)));
            entry_points.push_back(std::make_unique<imr::ShaderEntryPoint>(*modules.back(), stage, "main"));
            entry_point_ptrs.push_back(entry_points.back().get());
        }
        pipeline = std::make_unique<imr::GraphicsPipeline>(d, std::move(entry_point_ptrs), rts, stateBuilder);
    }
};

int main(int argc, char** argv) {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    auto window = glfwCreateWindow(1024, 1024, "Example", nullptr, nullptr);

    glfwSetKeyCallback(window, [](GLFWwindow* window, int key, int scancode, int action, int mods) {
        if (key == GLFW_KEY_R && (mods & GLFW_MOD_CONTROL))
            reload_shaders = true;
    });

    imr::Context context;
    imr::Device device(context, [&](vkb::PhysicalDeviceSelector& selector) {
        selector.add_required_extension(VK_EXT_MESH_SHADER_EXTENSION_NAME);

        VkPhysicalDeviceMeshShaderFeaturesEXT mesh_shader_features = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
            .taskShader = true,
            .meshShader = true,
        };
        selector.add_required_extension_features(mesh_shader_features);
    });
    imr::Swapchain swapchain(device, window);
    imr::FpsCounter fps_counter;

    std::unique_ptr<imr::Buffer> vertex_buffer;
    std::unique_ptr<imr::Buffer> face_buffer;
    std::unique_ptr<imr::Buffer> position_buffer;

    std::vector<vec4> vertices = {
        {0.0, 0.0, 0.0, 1.0}, // 0: A
        {1.0, 0.0, 0.0, 1.0}, // 1: B
        {1.0, 1.0, 0.0, 1.0}, // 2: C
        {0.0, 1.0, 0.0, 1.0}, // 3: D
        {0.0, 0.0, 1.0, 1.0}, // 4: E
        {1.0, 0.0, 1.0, 1.0}, // 5: F
        {1.0, 1.0, 1.0, 1.0}, // 6: G
        {0.0, 1.0, 1.0, 1.0}  // 7: H
    };

    std::vector<uvec4> faces = {
        {7, 3, 2, 6},   // top
        {0, 1, 2, 3},   // north
        {0, 3, 7, 4},   // west
        {5, 6, 2, 1},   // east
        {4, 7, 6, 5},   // south
        {4, 5, 1, 0}    // bottom
    };

    face_buffer = std::make_unique<imr::Buffer>(device, sizeof(vec4) * faces.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    face_buffer->uploadDataSync(0, face_buffer->size, faces.data());

    vertex_buffer = std::make_unique<imr::Buffer>(device, sizeof(vertices[0]) * vertices.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    vertex_buffer->uploadDataSync(0, vertex_buffer->size, vertices.data());

    push_constants_batched.vertex_buffer = vertex_buffer->device_address();
    push_constants_batched.face_buffer = face_buffer->device_address();

    std::vector<vec3> positions;
    for (size_t i = 0; i < INSTANCES_COUNT; i++) {
        vec3 p;
        p.x = ((float)rand() / RAND_MAX) * 20 - 10;
        p.y = ((float)rand() / RAND_MAX) * 20 - 10;
        p.z = ((float)rand() / RAND_MAX) * 20 - 10;
        positions.push_back(p);
    }

    position_buffer = std::make_unique<imr::Buffer>(device, sizeof(vec3) * positions.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    push_constants_batched.position_buffer = position_buffer->device_address();
    position_buffer->uploadDataSync(0, position_buffer->size, positions.data());

    auto prev_frame = imr_get_time_nano();
    float delta = 0;

    camera = {{0, 0, 3}, {0, 0}, 60};

    std::unique_ptr<imr::Image> depthBuffer;

    auto shaders = std::make_unique<Shaders>(device, swapchain);

    auto& vk = device.dispatch;
    while (!glfwWindowShouldClose(window)) {
        fps_counter.tick();
        fps_counter.updateGlfwWindowTitle(window);

        swapchain.renderFrameSimplified([&](imr::Swapchain::SimplifiedRenderContext& context) {
            camera_update(window, &camera_input);
            camera_move_freelook(&camera, &camera_input, &camera_state, delta);

            if (reload_shaders) {
                swapchain.drain();
                shaders = std::make_unique<Shaders>(device, swapchain);
                reload_shaders = false;
            }

            auto& image = context.image();
            auto cmdbuf = context.cmdbuf();

            if (!depthBuffer || depthBuffer->size().width != context.image().size().width || depthBuffer->size().height != context.image().size().height) {
                VkImageUsageFlagBits depthBufferFlags = static_cast<VkImageUsageFlagBits>(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
                depthBuffer = std::make_unique<imr::Image>(device, VK_IMAGE_TYPE_2D, context.image().size(), VK_FORMAT_D32_SFLOAT, depthBufferFlags);

                vk.cmdPipelineBarrier2KHR(cmdbuf, tmpPtr((VkDependencyInfo) {
                    .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                    .dependencyFlags = 0,
                    .imageMemoryBarrierCount = 1,
                    .pImageMemoryBarriers = tmpPtr((VkImageMemoryBarrier2) {
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                        .srcStageMask = 0,
                        .srcAccessMask = 0,
                        .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                        .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .image = depthBuffer->handle(),
                        .subresourceRange = depthBuffer->whole_image_subresource_range()
                    })
                }));
            }

            vk.cmdClearColorImage(cmdbuf, image.handle(), VK_IMAGE_LAYOUT_GENERAL, tmpPtr((VkClearColorValue) {
                .float32 = { 0.0f, 0.0f, 0.0f, 1.0f },
            }), 1, tmpPtr(image.whole_image_subresource_range()));

            vk.cmdClearDepthStencilImage(cmdbuf, depthBuffer->handle(), VK_IMAGE_LAYOUT_GENERAL, tmpPtr((VkClearDepthStencilValue) {
                .depth = 1.0f,
                .stencil = 0,
            }), 1, tmpPtr(depthBuffer->whole_image_subresource_range()));

            // This barrier ensures that the clear is finished before we run the dispatch.
            // before: all writes from the "transfer" stage (to which the clear command belongs)
            // after: all writes from the "compute" stage
            vk.cmdPipelineBarrier2KHR(cmdbuf, tmpPtr((VkDependencyInfo) {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .dependencyFlags = 0,
                .memoryBarrierCount = 1,
                .pMemoryBarriers = tmpPtr((VkMemoryBarrier2) {
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                    .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    .dstStageMask = VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
                    .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                })
            }));

            // update the push constant data on the host...
            mat4 m = identity_mat4;
            mat4 flip_y = identity_mat4;
            flip_y.rows[1][1] = -1;
            m = m * flip_y;
            mat4 view_mat = camera_get_view_mat4(&camera, context.image().size().width, context.image().size().height);
            m = m * view_mat;
            m = m * translate_mat4(vec3(-0.5, -0.5f, -0.5f));

            auto& pipeline = shaders->pipeline;
            vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->pipeline());

            context.frame().withRenderTargets(cmdbuf, { &image }, &*depthBuffer, [&]() {
                for (auto pos : positions) {
                    mat4 cube_matrix = m;
                    cube_matrix = cube_matrix * translate_mat4(pos);

                    push_constants_batched.matrix = cube_matrix;
                    vkCmdPushConstants(cmdbuf, pipeline->layout(), VK_SHADER_STAGE_MESH_BIT_EXT, 0, sizeof(push_constants_batched), &push_constants_batched);

                    vk.cmdDrawMeshTasksEXT(cmdbuf, 1, 1, 1);
                    //vkCmdDraw(cmdbuf, 12 * 3, 1, 0, 0);
                }
            });

            auto now = imr_get_time_nano();
            delta = ((float) ((now - prev_frame) / 1000L)) / 1000000.0f;
            prev_frame = now;

            glfwPollEvents();
        });
    }

    swapchain.drain();
    return 0;
}
