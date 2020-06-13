#include "rvpt.h"

#include <cstdlib>

#include <algorithm>
#include <fstream>
#include <random>

#include <glm/glm.hpp>
#include <glm/ext.hpp>
#include <nlohmann/json.hpp>
#include <imgui.h>

RVPT::RVPT(Window& window) : window_ref(window), scene_camera(window.get_aspect_ratio())
{
    std::ifstream input("project_configuration.json");
    nlohmann::json json;
    input >> json;
    if (json.contains("project_source_dir"))
    {
        source_folder = json["project_source_dir"];
    }
}

RVPT::~RVPT() {}

bool RVPT::initialize()
{
    bool init = context_init();
    pipeline_builder = VK::PipelineBuilder(vk_device, source_folder);
    memory_allocator =
        VK::MemoryAllocator(context.device.physical_device.physical_device, vk_device);

    init &= swapchain_init();
    for (uint32_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        sync_resources.emplace_back(vk_device, graphics_queue.value(), present_queue.value(),
                                    vkb_swapchain.swapchain);
    }
    frames_inflight_fences.resize(vkb_swapchain.image_count, nullptr);

    fullscreen_tri_render_pass = VK::create_render_pass(vk_device, vkb_swapchain.image_format);

    imgui_impl.emplace(vk_device, *graphics_queue, pipeline_builder, memory_allocator,
                       fullscreen_tri_render_pass, vkb_swapchain.extent, MAX_FRAMES_IN_FLIGHT);

    create_framebuffers();

    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            spheres.push_back({{2.f * i - 4.f, 1.f, 2.f * j - 4.f}, 1.f});
        }
    }

    rendering_resources = create_rendering_resources();

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        add_per_frame_data();
    }

    random_numbers.resize(1024);

    return init;
}
bool RVPT::update()
{
    // Generate random numbers
    std::mt19937 generator(std::random_device{}());
    std::uniform_real_distribution<float> distribution(0.0f, 1.0f);

    for (int i = 0; i < 1024; i++) random_numbers[i] = (distribution(generator));
    return true;
}

void RVPT::update_imgui()
{
    ImGuiIO& io = ImGui::GetIO();

    // Setup display size (every frame to accommodate for window resizing)
    int w, h;
    int display_w, display_h;
    auto win_ptr = window_ref.get_window_pointer();
    glfwGetWindowSize(win_ptr, &w, &h);
    glfwGetFramebufferSize(win_ptr, &display_w, &display_h);
    io.DisplaySize = ImVec2((float)w, (float)h);
    if (w > 0 && h > 0)
        io.DisplayFramebufferScale = ImVec2((float)display_w / w, (float)display_h / h);

    // Setup time step
    io.DeltaTime = static_cast<float>(time.since_last_frame());

    // imgui back end can't show 2 windows
    static bool show_stats = true;
    ImGui::SetWindowPos({0, 0});
    ImGui::SetWindowSize({160, 100});
    if (ImGui::Begin("Stats", &show_stats))
    {
        ImGui::Text("Frame Time %f", time.average_frame_time());
        ImGui::Text("FPS %f", 1.0 / time.average_frame_time());
    }
    ImGui::End();

    scene_camera.update_imgui();
}

RVPT::draw_return RVPT::draw()
{
    time.frame_start();
    per_frame_data[current_frame_index].raytrace_work_fence.wait();
    per_frame_data[current_frame_index].raytrace_work_fence.reset();

    per_frame_data[current_frame_index].camera_uniform.copy_to(scene_camera.get_data());
    per_frame_data[current_frame_index].random_uniform.copy_to(random_numbers);
    per_frame_data[current_frame_index].settings_uniform.copy_to(render_settings);

    per_frame_data[current_frame_index].sphere_buffer.copy_to(spheres);

    record_compute_command_buffer();

    VK::Queue& compute_submit = compute_queue.has_value() ? *compute_queue : *graphics_queue;
    compute_submit.submit(per_frame_data[current_frame_index].raytrace_command_buffer,
                          per_frame_data[current_frame_index].raytrace_work_fence);

    auto& current_frame = sync_resources[current_frame_index];

    current_frame.command_fence.wait();
    current_frame.command_buffer.reset();

    uint32_t swapchain_image_index;
    VkResult result = vkAcquireNextImageKHR(vk_device, vkb_swapchain.swapchain, UINT64_MAX,
                                            current_frame.image_avail_sem.get(), VK_NULL_HANDLE,
                                            &swapchain_image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR)
    {
        swapchain_reinit();
        return draw_return::swapchain_out_of_date;
    }
    else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
    {
        std::cerr << "Failed to acquire next swapchain image\n";
        assert(false);
    }
    record_command_buffer(current_frame, swapchain_image_index);

    if (frames_inflight_fences[swapchain_image_index] != nullptr)
    {
        vkWaitForFences(vk_device, 1, &frames_inflight_fences[swapchain_image_index], VK_TRUE,
                        UINT64_MAX);
    }
    frames_inflight_fences[swapchain_image_index] = current_frame.command_fence.get();

    current_frame.command_fence.reset();
    current_frame.submit();

    result = current_frame.present(swapchain_image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebuffer_resized)
    {
        framebuffer_resized = false;
        swapchain_reinit();
    }
    else if (result != VK_SUCCESS)
    {
        std::cerr << "Failed to present swapchain image\n";
        assert(false);
    }
    current_frame_index = (current_frame_index + 1) % sync_resources.size();
    time.frame_stop();
    return draw_return::success;
}

void RVPT::shutdown()
{
    if (compute_queue) compute_queue->wait_idle();
    graphics_queue->wait_idle();
    present_queue->wait_idle();

    per_frame_data.clear();
    rendering_resources.reset();

    imgui_impl.reset();

    VK::destroy_render_pass(vk_device, fullscreen_tri_render_pass);

    framebuffers.clear();

    sync_resources.clear();
    vkb_swapchain.destroy_image_views(swapchain_image_views);

    memory_allocator.shutdown();
    pipeline_builder.shutdown();
    vkb::destroy_swapchain(vkb_swapchain);
    vkb::destroy_device(context.device);
    vkDestroySurfaceKHR(context.inst.instance, context.surf, nullptr);
    vkb::destroy_instance(context.inst);
}

void replace_all(std::string& str, const std::string& from, const std::string& to)
{
    if (from.empty()) return;
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos)
    {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();  // In case 'to' contains 'from', like replacing 'x' with 'yx'
    }
}

void RVPT::reload_shaders()
{
    if (source_folder == "")
    {
        std::cout << "source_folder not set, unable to reload shaders\n";
        return;
    }

#ifdef WIN32
    auto double_backslash = source_folder;
    replace_all(double_backslash, "/", "\\\\");
    auto str = std::string("cd ") + double_backslash + "\\\\assets\\\\shaders && " +
               double_backslash + "\\\\scripts\\\\compile_shaders.bat";
    std::cout << str << "\n";
    std::system(str.c_str());
#elif __unix__
    auto str = std::string("cd ") + source_folder + "/assets/shaders && bash " + source_folder +
               "/scripts/compile_shaders.sh";
    std::system(str.c_str());
#endif
    if (compute_queue) compute_queue->wait_idle();
    graphics_queue->wait_idle();
    present_queue->wait_idle();

    pipeline_builder.recompile_pipelines();
}

// Private functions //
bool RVPT::context_init()
{
    vkb::InstanceBuilder inst_builder;
    auto inst_ret = inst_builder.set_app_name(window_ref.get_settings().title)
                        .request_validation_layers()
                        .use_default_debug_messenger()
                        .build();

    if (!inst_ret)
    {
        std::cerr << "Failed to create an instance: " << inst_ret.error().message() << '\n';
        return false;
    }

    context.inst = inst_ret.value();

    VkResult surf_res = glfwCreateWindowSurface(
        context.inst.instance, window_ref.get_window_pointer(), nullptr, &context.surf);
    if (surf_res != VK_SUCCESS)
    {
        std::cerr << "Failed to create a surface" << '\n';
        return false;
    }
    VkPhysicalDeviceFeatures required_features{};
    required_features.samplerAnisotropy = true;

    vkb::PhysicalDeviceSelector selector(context.inst);
    auto phys_ret = selector.set_surface(context.surf)
                        .set_required_features(required_features)
                        .set_minimum_version(1, 1)
                        .select();

    if (!phys_ret)
    {
        std::cerr << "Failed to find a physical device: " << phys_ret.error().message() << '\n';
        return false;
    }

    vkb::DeviceBuilder dev_builder(phys_ret.value());
    auto dev_ret = dev_builder.build();
    if (!dev_ret)
    {
        std::cerr << "Failed create a device: " << dev_ret.error().message() << '\n';
        return false;
    }

    context.device = dev_ret.value();
    vk_device = dev_ret.value().device;

    auto graphics_queue_index_ret = context.device.get_queue_index(vkb::QueueType::graphics);
    if (!graphics_queue_index_ret)
    {
        std::cerr << "Failed get the graphics queue: " << dev_ret.error().message() << '\n';

        return false;
    }
    graphics_queue.emplace(vk_device, graphics_queue_index_ret.value());

    auto present_queue_index_ret = context.device.get_queue_index(vkb::QueueType::present);
    if (!present_queue_index_ret)
    {
        std::cerr << "Failed get the present queue: " << dev_ret.error().message() << '\n';

        return false;
    }
    present_queue.emplace(vk_device, present_queue_index_ret.value());

    auto compute_queue_index_ret =
        context.device.get_dedicated_queue_index(vkb::QueueType::compute);
    if (compute_queue_index_ret)
    {
        compute_queue.emplace(vk_device, compute_queue_index_ret.value());
    }
    return true;
}

bool RVPT::swapchain_init()
{
    vkb::SwapchainBuilder swapchain_builder(context.device);
    auto ret = swapchain_builder.build();
    if (!ret)
    {
        std::cerr << "Failed to create a swapchain:" << ret.error().message() << '\n';
        return false;
    }
    vkb_swapchain = ret.value();
    return swapchain_get_images();
}

bool RVPT::swapchain_reinit()
{
    framebuffers.clear();
    vkb_swapchain.destroy_image_views(swapchain_image_views);

    vkb::SwapchainBuilder swapchain_builder(context.device);
    auto ret = swapchain_builder.set_old_swapchain(vkb_swapchain).build();
    if (!ret)
    {
        std::cerr << "Failed to recreate swapchain:" << ret.error().message() << '\n';
        return false;
    }
    vkb::destroy_swapchain(vkb_swapchain);
    vkb_swapchain = ret.value();
    bool out_bool = swapchain_get_images();
    create_framebuffers();
    return out_bool;
}

bool RVPT::swapchain_get_images()
{
    auto swapchain_images_ret = vkb_swapchain.get_images();
    if (!swapchain_images_ret)
    {
        return false;
    }
    swapchain_images = swapchain_images_ret.value();

    auto swapchain_image_views_ret = vkb_swapchain.get_image_views();
    if (!swapchain_image_views_ret)
    {
        return false;
    }
    swapchain_image_views = swapchain_image_views_ret.value();

    return true;
}

void RVPT::create_framebuffers()
{
    framebuffers.clear();
    for (uint32_t i = 0; i < vkb_swapchain.image_count; i++)
    {
        std::vector<VkImageView> image_views = {swapchain_image_views[i]};
        framebuffers.emplace_back(vk_device, fullscreen_tri_render_pass, vkb_swapchain.extent,
                                  image_views);
    }
}

RVPT::RenderingResources RVPT::create_rendering_resources()
{
    std::vector<VkDescriptorSetLayoutBinding> layout_bindings = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};

    auto image_pool = VK::DescriptorPool(vk_device, layout_bindings, MAX_FRAMES_IN_FLIGHT);

    std::vector<VkDescriptorSetLayoutBinding> compute_layout_bindings = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};

    auto raytrace_descriptor_pool =
        VK::DescriptorPool(vk_device, compute_layout_bindings, MAX_FRAMES_IN_FLIGHT);

    auto fullscreen_triangle_pipeline = pipeline_builder.create_graphics_pipeline(
        "fullscreen_tri.vert.spv", "tex_sample.frag.spv", {image_pool.layout()}, {},
        fullscreen_tri_render_pass, vkb_swapchain.extent);

    auto raytrace_pipeline = pipeline_builder.create_compute_pipeline(
        "compute_pass.comp.spv", {raytrace_descriptor_pool.layout()}, {});

    return RVPT::RenderingResources{std::move(image_pool), std::move(raytrace_descriptor_pool),
                                    fullscreen_triangle_pipeline, raytrace_pipeline};
}

void RVPT::add_per_frame_data()
{
    auto output_image = VK::Image(
        vk_device, memory_allocator, *graphics_queue, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TILING_OPTIMAL, 512, 512, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
        VK_IMAGE_LAYOUT_GENERAL, static_cast<VkDeviceSize>(512 * 512 * 4), VK::MemoryUsage::gpu);
    auto camera_uniform =
        VK::Buffer(vk_device, memory_allocator, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 64,
                   VK::MemoryUsage::cpu_to_gpu);
    auto random_uniform =
        VK::Buffer(vk_device, memory_allocator, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 4096,
                   VK::MemoryUsage::cpu_to_gpu);
    auto settings_uniform =
        VK::Buffer(vk_device, memory_allocator, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 8,
                   VK::MemoryUsage::cpu_to_gpu);
    auto sphere_buffer = VK::Buffer(vk_device, memory_allocator, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                    sizeof(Sphere) * spheres.size(), VK::MemoryUsage::cpu_to_gpu);
    auto raytrace_command_buffer =
        VK::CommandBuffer(vk_device, compute_queue.has_value() ? *compute_queue : *graphics_queue);
    auto raytrace_work_fence = VK::Fence(vk_device);

    // descriptor sets
    auto image_descriptor_set = VK::DescriptorSet(rendering_resources->image_pool.allocate());
    auto raytracing_descriptor_set =
        VK::DescriptorSet(rendering_resources->raytrace_descriptor_pool.allocate());

    // update descriptor sets with resources
    std::vector<VkDescriptorImageInfo> image_descriptor_info = {output_image.descriptor_info()};
    VK::DescriptorUse descriptor_use{0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     image_descriptor_info};

    auto write_descriptor = descriptor_use.get_write_descriptor_set(image_descriptor_set.set);
    vkUpdateDescriptorSets(vk_device, 1, &write_descriptor, 0, nullptr);

    VK::DescriptorUse image_descriptor_use{0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                           image_descriptor_info};

    std::vector<VkDescriptorBufferInfo> camera_buffer_descriptor_info = {
        camera_uniform.descriptor_info()};
    VK::DescriptorUse camera_buffer_descriptor_use{1, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                   camera_buffer_descriptor_info};

    std::vector<VkDescriptorBufferInfo> random_buffer_descriptor_info = {
        random_uniform.descriptor_info()};
    VK::DescriptorUse random_buffer_descriptor_use{2, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                   random_buffer_descriptor_info};

    std::vector<VkDescriptorBufferInfo> frame_settings_descriptor_info = {
        settings_uniform.descriptor_info()};
    VK::DescriptorUse frame_settings_descriptor_use{3, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                    frame_settings_descriptor_info};

    std::vector<VkDescriptorBufferInfo> sphere_buffer_descriptor_info = {
        sphere_buffer.descriptor_info()};
    VK::DescriptorUse sphere_buffer_descriptor_use{4, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                   sphere_buffer_descriptor_info};

    std::vector<VkWriteDescriptorSet> write_descriptors = {
        image_descriptor_use.get_write_descriptor_set(raytracing_descriptor_set.set),
        camera_buffer_descriptor_use.get_write_descriptor_set(raytracing_descriptor_set.set),
        random_buffer_descriptor_use.get_write_descriptor_set(raytracing_descriptor_set.set),
        frame_settings_descriptor_use.get_write_descriptor_set(raytracing_descriptor_set.set),
        sphere_buffer_descriptor_use.get_write_descriptor_set(raytracing_descriptor_set.set)};

    vkUpdateDescriptorSets(vk_device, write_descriptors.size(), write_descriptors.data(), 0,
                           nullptr);
    per_frame_data.push_back(RVPT::PerFrameData{
        std::move(output_image), std::move(camera_uniform), std::move(random_uniform),
        std::move(settings_uniform), std::move(sphere_buffer), std::move(raytrace_command_buffer),
        std::move(raytrace_work_fence), image_descriptor_set, raytracing_descriptor_set});
}

void RVPT::record_command_buffer(VK::SyncResources& current_frame, uint32_t swapchain_image_index)
{
    current_frame.command_buffer.begin();
    VkCommandBuffer cmd_buf = current_frame.command_buffer.get();

    // Image memory barrier to make sure that compute shader writes are
    // finished before sampling from the texture
    VkImageMemoryBarrier imageMemoryBarrier = {};
    imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    imageMemoryBarrier.image = per_frame_data[current_frame_index].output_image.image.handle;
    imageMemoryBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    imageMemoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    imageMemoryBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd_buf, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK::FLAGS_NONE, 0, nullptr, 0,
                         nullptr, 1, &imageMemoryBarrier);

    VkClearValue background_color = {0.0f, 0.0f, 0.0f, 1.0f};

    VkRenderPassBeginInfo rp_begin_info{};
    rp_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin_info.renderPass = fullscreen_tri_render_pass;
    rp_begin_info.framebuffer = framebuffers.at(swapchain_image_index).framebuffer.handle;
    rp_begin_info.renderArea.offset = {0, 0};
    rp_begin_info.renderArea.extent = vkb_swapchain.extent;
    rp_begin_info.clearValueCount = 1;
    rp_begin_info.pClearValues = &background_color;

    vkCmdBeginRenderPass(cmd_buf, &rp_begin_info, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{0.0f,
                        0.0f,
                        static_cast<float>(vkb_swapchain.extent.width),
                        static_cast<float>(vkb_swapchain.extent.height),
                        0.0f,
                        1.0f};
    vkCmdSetViewport(cmd_buf, 0, 1, &viewport);

    VkRect2D scissor{{0, 0}, vkb_swapchain.extent};
    vkCmdSetScissor(cmd_buf, 0, 1, &scissor);

    pipeline_builder.get_pipeline(rendering_resources->fullscreen_triangle_pipeline);

    vkCmdBindPipeline(
        cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_builder.get_pipeline(rendering_resources->fullscreen_triangle_pipeline));
    vkCmdBindDescriptorSets(
        cmd_buf, VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_builder.get_layout(rendering_resources->fullscreen_triangle_pipeline), 0, 1,
        &per_frame_data[current_frame_index].image_descriptor_set.set, 0, nullptr);
    vkCmdDraw(cmd_buf, 3, 1, 0, 0);

    imgui_impl->draw(cmd_buf, current_frame_index);

    vkCmdEndRenderPass(cmd_buf);
    current_frame.command_buffer.end();
}

void RVPT::record_compute_command_buffer()
{
    auto& command_buffer = per_frame_data[current_frame_index].raytrace_command_buffer;
    command_buffer.begin();
    VkCommandBuffer cmd_buf = command_buffer.get();
    vkCmdBindPipeline(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline_builder.get_pipeline(rendering_resources->raytrace_pipeline));
    vkCmdBindDescriptorSets(cmd_buf, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_builder.get_layout(rendering_resources->raytrace_pipeline), 0,
                            1, &per_frame_data[current_frame_index].raytracing_descriptor_sets.set,
                            0, 0);

    vkCmdDispatch(cmd_buf, per_frame_data[current_frame_index].output_image.width / 16,
                  per_frame_data[current_frame_index].output_image.height / 16, 1);

    command_buffer.end();
}
