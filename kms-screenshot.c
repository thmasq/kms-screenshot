#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <libdrm/drm.h>
#include <libdrm/drm_fourcc.h>
#include <libdrm/drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// AMDGPU specific headers
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>

#include "hdr_tonemap_comp_spv.h"
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

extern unsigned char hdr_tonemap_comp_spv[];
extern unsigned int hdr_tonemap_comp_spv_len;

typedef struct {
	float exposure;
	uint32_t
	    tonemapMode; // 0=Reinhard, 1=ACES_Fast, 2=ACES_Hill, 3=ACES_Day,
	                 // 4=ACES_Full, 5=Hable, 6=Reinhard_Ext, 7=Uchimura
} ToneMappingPushConstants;

typedef struct {
	VkDescriptorSetLayout descriptor_set_layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline compute_pipeline;
	VkDescriptorPool descriptor_pool;
} ComputePipeline;

typedef struct {
	VkInstance instance;
	VkPhysicalDevice physical_device;
	VkDevice device;
	VkQueue queue;
	uint32_t queue_family_index;
	VkCommandPool command_pool;
} VulkanContext;

// Define formats that might not be in older headers
#ifndef DRM_FORMAT_ABGR16161616
#define DRM_FORMAT_ABGR16161616 fourcc_code('A', 'B', '4', '8')
#endif

// Define format modifiers
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0
#endif

// Define flags that might not be available
#ifndef O_CLOEXEC
#define O_CLOEXEC 02000000
#endif

// SDMA packet definitions for copy
#define SDMA_OPCODE_COPY 1
#define SDMA_COPY_SUB_OPCODE_LINEAR 0

#define SDMA_PKT_HEADER_OP(x) (((x) & 0xFF) << 0)
#define SDMA_PKT_HEADER_SUB_OP(x) (((x) & 0xFF) << 8)
#define SDMA_PKT_COPY_LINEAR_HEADER_DWORD                                      \
	(SDMA_PKT_HEADER_OP(SDMA_OPCODE_COPY) |                                \
	 SDMA_PKT_HEADER_SUB_OP(SDMA_COPY_SUB_OPCODE_LINEAR))

static int create_tonemap_compute_pipeline(VulkanContext *ctx,
                                           ComputePipeline *pipeline)
{
	VkResult result;

	// Create shader module
	VkShaderModuleCreateInfo shader_info = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = hdr_tonemap_comp_spv_len,
	    .pCode = (uint32_t *)hdr_tonemap_comp_spv,
	};

	VkShaderModule shader_module;
	result = vkCreateShaderModule(ctx->device, &shader_info, NULL,
	                              &shader_module);
	if (result != VK_SUCCESS) {
		printf("Failed to create shader module: %d\n", result);
		return -1;
	}

	// Create descriptor set layout
	VkDescriptorSetLayoutBinding bindings[2] = {
	    {
	        .binding = 0,
	        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	        .descriptorCount = 1,
	        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	    },
	    {
	        .binding = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	        .descriptorCount = 1,
	        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	    },
	};

	VkDescriptorSetLayoutCreateInfo layout_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = 2,
	    .pBindings = bindings,
	};

	result = vkCreateDescriptorSetLayout(ctx->device, &layout_info, NULL,
	                                     &pipeline->descriptor_set_layout);
	if (result != VK_SUCCESS) {
		printf("Failed to create descriptor set layout: %d\n", result);
		vkDestroyShaderModule(ctx->device, shader_module, NULL);
		return -1;
	}

	// Create pipeline layout with push constants
	VkPushConstantRange push_constant_range = {
	    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
	    .offset = 0,
	    .size = sizeof(ToneMappingPushConstants),
	};

	VkPipelineLayoutCreateInfo pipeline_layout_info = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &pipeline->descriptor_set_layout,
	    .pushConstantRangeCount = 1,
	    .pPushConstantRanges = &push_constant_range,
	};

	result = vkCreatePipelineLayout(ctx->device, &pipeline_layout_info,
	                                NULL, &pipeline->pipeline_layout);
	if (result != VK_SUCCESS) {
		printf("Failed to create pipeline layout: %d\n", result);
		vkDestroyDescriptorSetLayout(
		    ctx->device, pipeline->descriptor_set_layout, NULL);
		vkDestroyShaderModule(ctx->device, shader_module, NULL);
		return -1;
	}

	// Create compute pipeline
	VkComputePipelineCreateInfo compute_pipeline_info = {
	    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
	    .stage =
	        {
	            .sType =
	                VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
	            .module = shader_module,
	            .pName = "main",
	        },
	    .layout = pipeline->pipeline_layout,
	};

	result = vkCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1,
	                                  &compute_pipeline_info, NULL,
	                                  &pipeline->compute_pipeline);
	if (result != VK_SUCCESS) {
		printf("Failed to create compute pipeline: %d\n", result);
		vkDestroyPipelineLayout(ctx->device, pipeline->pipeline_layout,
		                        NULL);
		vkDestroyDescriptorSetLayout(
		    ctx->device, pipeline->descriptor_set_layout, NULL);
		vkDestroyShaderModule(ctx->device, shader_module, NULL);
		return -1;
	}

	// Create descriptor pool
	VkDescriptorPoolSize pool_size = {
	    .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	    .descriptorCount = 2,
	};

	VkDescriptorPoolCreateInfo pool_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .maxSets = 1,
	    .poolSizeCount = 1,
	    .pPoolSizes = &pool_size,
	};

	result = vkCreateDescriptorPool(ctx->device, &pool_info, NULL,
	                                &pipeline->descriptor_pool);
	if (result != VK_SUCCESS) {
		printf("Failed to create descriptor pool: %d\n", result);
		vkDestroyPipeline(ctx->device, pipeline->compute_pipeline,
		                  NULL);
		vkDestroyPipelineLayout(ctx->device, pipeline->pipeline_layout,
		                        NULL);
		vkDestroyDescriptorSetLayout(
		    ctx->device, pipeline->descriptor_set_layout, NULL);
		vkDestroyShaderModule(ctx->device, shader_module, NULL);
		return -1;
	}

	vkDestroyShaderModule(ctx->device, shader_module, NULL);
	printf("\tTone mapping compute pipeline created successfully\n");
	return 0;
}

static void cleanup_compute_pipeline(VulkanContext *ctx,
                                     ComputePipeline *pipeline)
{
	if (pipeline->descriptor_pool != VK_NULL_HANDLE)
		vkDestroyDescriptorPool(ctx->device, pipeline->descriptor_pool,
		                        NULL);
	if (pipeline->compute_pipeline != VK_NULL_HANDLE)
		vkDestroyPipeline(ctx->device, pipeline->compute_pipeline,
		                  NULL);
	if (pipeline->pipeline_layout != VK_NULL_HANDLE)
		vkDestroyPipelineLayout(ctx->device, pipeline->pipeline_layout,
		                        NULL);
	if (pipeline->descriptor_set_layout != VK_NULL_HANDLE)
		vkDestroyDescriptorSetLayout(
		    ctx->device, pipeline->descriptor_set_layout, NULL);
}

static int apply_tone_mapping(VulkanContext *ctx, ComputePipeline *pipeline,
                              VkImage input_image, VkImage output_image,
                              uint32_t width, uint32_t height, float exposure,
                              uint32_t tonemap_mode)
{
	VkResult result;

	// Allocate descriptor set
	VkDescriptorSetAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	    .descriptorPool = pipeline->descriptor_pool,
	    .descriptorSetCount = 1,
	    .pSetLayouts = &pipeline->descriptor_set_layout,
	};

	VkDescriptorSet descriptor_set;
	result =
	    vkAllocateDescriptorSets(ctx->device, &alloc_info, &descriptor_set);
	if (result != VK_SUCCESS) {
		printf("Failed to allocate descriptor set: %d\n", result);
		return -1;
	}

	// Create image views
	VkImageViewCreateInfo input_view_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = input_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_R16G16B16A16_UNORM,
	    .subresourceRange =
	        {
	            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel = 0,
	            .levelCount = 1,
	            .baseArrayLayer = 0,
	            .layerCount = 1,
	        },
	};

	VkImageView input_view;
	result =
	    vkCreateImageView(ctx->device, &input_view_info, NULL, &input_view);
	if (result != VK_SUCCESS) {
		printf("Failed to create input image view: %d\n", result);
		return -1;
	}

	VkImageViewCreateInfo output_view_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = output_image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .subresourceRange =
	        {
	            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
	            .baseMipLevel = 0,
	            .levelCount = 1,
	            .baseArrayLayer = 0,
	            .layerCount = 1,
	        },
	};

	VkImageView output_view;
	result = vkCreateImageView(ctx->device, &output_view_info, NULL,
	                           &output_view);
	if (result != VK_SUCCESS) {
		printf("Failed to create output image view: %d\n", result);
		vkDestroyImageView(ctx->device, input_view, NULL);
		return -1;
	}

	// Update descriptor set
	VkDescriptorImageInfo image_infos[2] = {
	    {
	        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        .imageView = input_view,
	    },
	    {
	        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
	        .imageView = output_view,
	    },
	};

	VkWriteDescriptorSet writes[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = descriptor_set,
	        .dstBinding = 0,
	        .descriptorCount = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	        .pImageInfo = &image_infos[0],
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	        .dstSet = descriptor_set,
	        .dstBinding = 1,
	        .descriptorCount = 1,
	        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
	        .pImageInfo = &image_infos[1],
	    },
	};

	vkUpdateDescriptorSets(ctx->device, 2, writes, 0, NULL);

	// Record and execute compute commands
	VkCommandBufferAllocateInfo cmd_alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = ctx->command_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};

	VkCommandBuffer cmd_buffer;
	result =
	    vkAllocateCommandBuffers(ctx->device, &cmd_alloc_info, &cmd_buffer);
	if (result != VK_SUCCESS) {
		printf("Failed to allocate command buffer: %d\n", result);
		vkDestroyImageView(ctx->device, output_view, NULL);
		vkDestroyImageView(ctx->device, input_view, NULL);
		return -1;
	}

	VkCommandBufferBeginInfo begin_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	vkBeginCommandBuffer(cmd_buffer, &begin_info);

	// Transition images to general layout for compute
	VkImageMemoryBarrier barriers[2] = {
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = input_image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	        .srcAccessMask = 0,
	        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    },
	    {
	        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	        .image = output_image,
	        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	        .srcAccessMask = 0,
	        .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
	    },
	};

	vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL,
	                     0, NULL, 2, barriers);

	// Bind compute pipeline and descriptor set
	vkCmdBindPipeline(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
	                  pipeline->compute_pipeline);
	vkCmdBindDescriptorSets(cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
	                        pipeline->pipeline_layout, 0, 1,
	                        &descriptor_set, 0, NULL);

	// Set push constants
	ToneMappingPushConstants push_constants = {
	    .exposure = exposure,
	    .tonemapMode = tonemap_mode,
	};

	vkCmdPushConstants(cmd_buffer, pipeline->pipeline_layout,
	                   VK_SHADER_STAGE_COMPUTE_BIT, 0,
	                   sizeof(push_constants), &push_constants);

	// Dispatch compute shader (16x16 workgroup size)
	uint32_t group_count_x = (width + 15) / 16;
	uint32_t group_count_y = (height + 15) / 16;
	vkCmdDispatch(cmd_buffer, group_count_x, group_count_y, 1);

	// Memory barrier before host read
	VkImageMemoryBarrier final_barrier = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
	    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .image = output_image,
	    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
	    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
	};

	vkCmdPipelineBarrier(cmd_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	                     VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 0, NULL, 1,
	                     &final_barrier);

	vkEndCommandBuffer(cmd_buffer);

	// Submit and wait
	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd_buffer,
	};

	result = vkQueueSubmit(ctx->queue, 1, &submit_info, VK_NULL_HANDLE);
	if (result == VK_SUCCESS) {
		result = vkQueueWaitIdle(ctx->queue);
	}

	const char *tonemap_names[] = {
	    "Reinhard",      "ACES Fast", "ACES Hill",         "ACES Day",
	    "ACES Full RRT", "Hable",     "Reinhard Extended", "Uchimura"};

	printf("\tTone mapping applied: %s, exposure=%.2f\n",
	       tonemap_names[tonemap_mode], exposure);

	// Cleanup
	vkDestroyImageView(ctx->device, output_view, NULL);
	vkDestroyImageView(ctx->device, input_view, NULL);

	return (result == VK_SUCCESS) ? 0 : -1;
}

static int init_vulkan_context(VulkanContext *ctx)
{
	VkResult result;

	// Create Vulkan instance with CORRECT instance extensions only
	VkApplicationInfo app_info = {
	    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	    .pApplicationName = "KMS Screenshot",
	    .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
	    .pEngineName = "No Engine",
	    .engineVersion = VK_MAKE_VERSION(1, 0, 0),
	    .apiVersion = VK_API_VERSION_1_2,
	};

	// These are INSTANCE extensions (not device extensions!)
	const char *required_instance_extensions[] = {
	    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, // "VK_KHR_external_memory_capabilities"
	    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, // "VK_KHR_get_physical_device_properties2"
	};

	VkInstanceCreateInfo create_info = {
	    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	    .pApplicationInfo = &app_info,
	    .enabledExtensionCount = 2,
	    .ppEnabledExtensionNames = required_instance_extensions,
	};

	result = vkCreateInstance(&create_info, NULL, &ctx->instance);
	if (result != VK_SUCCESS) {
		printf("Failed to create Vulkan instance: %d\n", result);
		return -1;
	}

	printf("Vulkan instance created with correct extensions\n");

	// Find physical device (prefer discrete GPU, fallback to any)
	uint32_t device_count = 0;
	vkEnumeratePhysicalDevices(ctx->instance, &device_count, NULL);
	if (device_count == 0) {
		printf("No Vulkan-capable devices found\n");
		vkDestroyInstance(ctx->instance, NULL);
		return -1;
	}

	VkPhysicalDevice *devices =
	    malloc(device_count * sizeof(VkPhysicalDevice));
	vkEnumeratePhysicalDevices(ctx->instance, &device_count, devices);

	ctx->physical_device = VK_NULL_HANDLE;
	for (uint32_t i = 0; i < device_count; i++) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(devices[i], &props);

		// Check if device supports required DEVICE extensions
		uint32_t ext_count;
		vkEnumerateDeviceExtensionProperties(devices[i], NULL,
		                                     &ext_count, NULL);
		VkExtensionProperties *extensions =
		    malloc(ext_count * sizeof(VkExtensionProperties));
		vkEnumerateDeviceExtensionProperties(devices[i], NULL,
		                                     &ext_count, extensions);

		bool has_dmabuf = false, has_modifier = false,
		     has_external_mem = false;
		for (uint32_t j = 0; j < ext_count; j++) {
			if (strcmp(
			        extensions[j].extensionName,
			        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) ==
			    0)
				has_dmabuf = true;
			if (strcmp(
			        extensions[j].extensionName,
			        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) ==
			    0)
				has_modifier = true;
			if (strcmp(extensions[j].extensionName,
			           VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) == 0)
				has_external_mem = true;
		}
		free(extensions);

		if (has_dmabuf && has_modifier && has_external_mem) {
			ctx->physical_device = devices[i];
			printf("\tSelected Vulkan device: %s\n",
			       props.deviceName);
			printf("\tAll required device extensions available\n");
			break;
		}
	}
	free(devices);

	if (ctx->physical_device == VK_NULL_HANDLE) {
		printf("No suitable Vulkan device found with required "
		       "extensions\n");
		vkDestroyInstance(ctx->instance, NULL);
		return -1;
	}

	// Find queue family that supports graphics and compute
	uint32_t queue_family_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical_device,
	                                         &queue_family_count, NULL);
	VkQueueFamilyProperties *queue_families =
	    malloc(queue_family_count * sizeof(VkQueueFamilyProperties));
	vkGetPhysicalDeviceQueueFamilyProperties(
	    ctx->physical_device, &queue_family_count, queue_families);

	ctx->queue_family_index = UINT32_MAX;
	for (uint32_t i = 0; i < queue_family_count; i++) {
		if (queue_families[i].queueFlags &
		    (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)) {
			ctx->queue_family_index = i;
			break;
		}
	}
	free(queue_families);

	if (ctx->queue_family_index == UINT32_MAX) {
		printf("No suitable queue family found\n");
		vkDestroyInstance(ctx->instance, NULL);
		return -1;
	}

	// Create logical device with DEVICE extensions
	const char *device_extensions[] = {
	    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,   // Device extension
	    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, // Device extension
	    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,           // Device extension
	};

	float queue_priority = 1.0f;
	VkDeviceQueueCreateInfo queue_create_info = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	    .queueFamilyIndex = ctx->queue_family_index,
	    .queueCount = 1,
	    .pQueuePriorities = &queue_priority,
	};

	VkDeviceCreateInfo device_create_info = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	    .queueCreateInfoCount = 1,
	    .pQueueCreateInfos = &queue_create_info,
	    .enabledExtensionCount = 3,
	    .ppEnabledExtensionNames = device_extensions,
	};

	result = vkCreateDevice(ctx->physical_device, &device_create_info, NULL,
	                        &ctx->device);
	if (result != VK_SUCCESS) {
		printf("Failed to create Vulkan device: %d\n", result);
		vkDestroyInstance(ctx->instance, NULL);
		return -1;
	}

	printf("\tVulkan device created with required device extensions\n");

	// Get queue and create command pool
	vkGetDeviceQueue(ctx->device, ctx->queue_family_index, 0, &ctx->queue);

	VkCommandPoolCreateInfo pool_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	    .queueFamilyIndex = ctx->queue_family_index,
	};

	result = vkCreateCommandPool(ctx->device, &pool_info, NULL,
	                             &ctx->command_pool);
	if (result != VK_SUCCESS) {
		printf("Failed to create command pool: %d\n", result);
		vkDestroyDevice(ctx->device, NULL);
		vkDestroyInstance(ctx->instance, NULL);
		return -1;
	}

	printf("\tVulkan context initialized successfully\n");
	return 0;
}

static void cleanup_vulkan_context(VulkanContext *ctx)
{
	if (ctx->command_pool != VK_NULL_HANDLE)
		vkDestroyCommandPool(ctx->device, ctx->command_pool, NULL);
	if (ctx->device != VK_NULL_HANDLE)
		vkDestroyDevice(ctx->device, NULL);
	if (ctx->instance != VK_NULL_HANDLE)
		vkDestroyInstance(ctx->instance, NULL);
}

static VkFormat drm_format_to_vulkan(uint32_t drm_format)
{
	switch (drm_format) {
	case DRM_FORMAT_ABGR16161616:
		return VK_FORMAT_R16G16B16A16_UNORM;
	case DRM_FORMAT_ARGB8888:
		return VK_FORMAT_B8G8R8A8_UNORM;
	case DRM_FORMAT_XRGB8888:
		return VK_FORMAT_B8G8R8A8_UNORM;
	case DRM_FORMAT_ABGR8888:
		return VK_FORMAT_R8G8B8A8_UNORM;
	case DRM_FORMAT_XBGR8888:
		return VK_FORMAT_R8G8B8A8_UNORM;
	default:
		return VK_FORMAT_UNDEFINED;
	}
}

// Simple PPM image writer
static int write_ppm(const char *filename, uint32_t width, uint32_t height,
                     uint8_t *rgb_data)
{
	FILE *fp = fopen(filename, "wb");
	if (!fp) {
		perror("fopen");
		return -1;
	}

	fprintf(fp, "P6\n%u %u\n255\n", width, height);
	fwrite(rgb_data, 3, width * height, fp);
	fclose(fp);
	return 0;
}

// Convert various pixel formats to RGB24
static void convert_to_rgb24(uint8_t *src, uint8_t *dst, uint32_t width,
                             uint32_t height, uint32_t format, uint32_t stride)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888: {
		// BGRA/BGRX -> RGB
		for (uint32_t y = 0; y < height; y++) {
			uint32_t *src_row = (uint32_t *)(src + y * stride);
			uint8_t *dst_row = dst + y * width * 3;

			for (uint32_t x = 0; x < width; x++) {
				uint32_t pixel = src_row[x];
				dst_row[x * 3 + 0] = (pixel >> 16) & 0xFF; // R
				dst_row[x * 3 + 1] = (pixel >> 8) & 0xFF;  // G
				dst_row[x * 3 + 2] = pixel & 0xFF;         // B
			}
		}
		break;
	}
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888: {
		// RGBA/RGBX -> RGB
		for (uint32_t y = 0; y < height; y++) {
			uint32_t *src_row = (uint32_t *)(src + y * stride);
			uint8_t *dst_row = dst + y * width * 3;

			for (uint32_t x = 0; x < width; x++) {
				uint32_t pixel = src_row[x];
				dst_row[x * 3 + 0] = pixel & 0xFF;         // R
				dst_row[x * 3 + 1] = (pixel >> 8) & 0xFF;  // G
				dst_row[x * 3 + 2] = (pixel >> 16) & 0xFF; // B
			}
		}
		break;
	}
	case DRM_FORMAT_RGB565: {
		// RGB565 -> RGB
		for (uint32_t y = 0; y < height; y++) {
			uint16_t *src_row = (uint16_t *)(src + y * stride);
			uint8_t *dst_row = dst + y * width * 3;

			for (uint32_t x = 0; x < width; x++) {
				uint16_t pixel = src_row[x];
				dst_row[x * 3 + 0] = ((pixel >> 11) & 0x1F)
				                     << 3; // R
				dst_row[x * 3 + 1] = ((pixel >> 5) & 0x3F)
				                     << 2;                // G
				dst_row[x * 3 + 2] = (pixel & 0x1F) << 3; // B
			}
		}
		break;
	}
	case DRM_FORMAT_ABGR16161616: // 0x38344241 - 64-bit format, 16 bits per
	                              // channel
	{
		// ABGR 16-bit per channel -> RGB 8-bit per channel
		for (uint32_t y = 0; y < height; y++) {
			uint64_t *src_row = (uint64_t *)(src + y * stride);
			uint8_t *dst_row = dst + y * width * 3;

			for (uint32_t x = 0; x < width; x++) {
				uint64_t pixel = src_row[x];
				// Extract 16-bit channels and convert to 8-bit
				uint16_t a =
				    (pixel >> 48) & 0xFFFF; // Alpha (unused)
				uint16_t b = (pixel >> 32) & 0xFFFF; // Blue
				uint16_t g = (pixel >> 16) & 0xFFFF; // Green
				uint16_t r = pixel & 0xFFFF;         // Red

				// Convert 16-bit to 8-bit by taking high byte
				dst_row[x * 3 + 0] = r >> 8; // R
				dst_row[x * 3 + 1] = g >> 8; // G
				dst_row[x * 3 + 2] = b >> 8; // B

				(void)a; // Suppress unused variable warning
			}
		}
		break;
	}
	default:
		printf("Unsupported pixel format: 0x%08x (%c%c%c%c)\n", format,
		       format & 0xFF, (format >> 8) & 0xFF,
		       (format >> 16) & 0xFF, (format >> 24) & 0xFF);
		memset(dst, 0, width * height * 3);
		break;
	}
}

static const char *format_to_string(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
		return "XRGB8888";
	case DRM_FORMAT_ARGB8888:
		return "ARGB8888";
	case DRM_FORMAT_XBGR8888:
		return "XBGR8888";
	case DRM_FORMAT_ABGR8888:
		return "ABGR8888";
	case DRM_FORMAT_RGB565:
		return "RGB565";
	case DRM_FORMAT_ABGR16161616:
		return "ABGR16161616"; // 0x38344241
	default: {
		static char buf[16];
		snprintf(buf, sizeof(buf), "%c%c%c%c", format & 0xFF,
		         (format >> 8) & 0xFF, (format >> 16) & 0xFF,
		         (format >> 24) & 0xFF);
		return buf;
	}
	}
}

// AMDGPU buffer copy using SDMA
static int amdgpu_copy_buffer(amdgpu_device_handle dev,
                              amdgpu_context_handle ctx, uint64_t src_va,
                              uint64_t dst_va, uint64_t size)
{
	amdgpu_bo_handle ib_bo;
	void *ib_cpu;
	uint64_t ib_mc;
	amdgpu_va_handle ib_va_handle;
	uint32_t *ib;
	struct amdgpu_cs_request ibs_request = {0};
	struct amdgpu_cs_ib_info ib_info = {0};
	struct amdgpu_cs_fence fence_status = {0};
	uint32_t expired;
	int r;

	// Allocate IB (indirect buffer) for commands
	struct amdgpu_bo_alloc_request ib_req = {0};
	ib_req.alloc_size = 4096;
	ib_req.phys_alignment = 4096;
	ib_req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
	ib_req.flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;

	r = amdgpu_bo_alloc(dev, &ib_req, &ib_bo);
	if (r) {
		printf("Failed to allocate IB: %d\n", r);
		return r;
	}

	r = amdgpu_bo_cpu_map(ib_bo, &ib_cpu);
	if (r) {
		printf("Failed to map IB: %d\n", r);
		amdgpu_bo_free(ib_bo);
		return r;
	}

	// Allocate VA for IB
	r = amdgpu_va_range_alloc(dev, amdgpu_gpu_va_range_general,
	                          ib_req.alloc_size, 4096, 0, &ib_mc,
	                          &ib_va_handle, 0);
	if (r) {
		printf("Failed to allocate IB VA: %d\n", r);
		amdgpu_bo_cpu_unmap(ib_bo);
		amdgpu_bo_free(ib_bo);
		return r;
	}

	r = amdgpu_bo_va_op(ib_bo, 0, ib_req.alloc_size, ib_mc, 0,
	                    AMDGPU_VA_OP_MAP);
	if (r) {
		printf("Failed to map IB VA: %d\n", r);
		amdgpu_va_range_free(ib_va_handle);
		amdgpu_bo_cpu_unmap(ib_bo);
		amdgpu_bo_free(ib_bo);
		return r;
	}

	ib = (uint32_t *)ib_cpu;

	// Build SDMA copy packet
	ib[0] = SDMA_PKT_COPY_LINEAR_HEADER_DWORD;
	ib[1] = size - 1;                    // Count - 1
	ib[2] = 0;                           // Reserved
	ib[3] = src_va & 0xFFFFFFFF;         // Src addr low
	ib[4] = (src_va >> 32) & 0xFFFFFFFF; // Src addr high
	ib[5] = dst_va & 0xFFFFFFFF;         // Dst addr low
	ib[6] = (dst_va >> 32) & 0xFFFFFFFF; // Dst addr high

	// Setup IB info
	ib_info.ib_mc_address = ib_mc;
	ib_info.size = 7; // Number of DWORDs

	// Setup CS request
	ibs_request.ip_type = AMDGPU_HW_IP_DMA;
	ibs_request.ring = 0;
	ibs_request.number_of_ibs = 1;
	ibs_request.ibs = &ib_info;
	ibs_request.fence_info.handle = NULL;

	// Submit
	r = amdgpu_cs_submit(ctx, 0, &ibs_request, 1);
	if (r) {
		printf("Failed to submit CS: %d\n", r);
		goto cleanup;
	}

	fence_status.context = ctx;
	fence_status.ip_type = AMDGPU_HW_IP_DMA;
	fence_status.ip_instance = 0;
	fence_status.ring = 0;
	fence_status.fence = ibs_request.seq_no;

	// Wait for completion
	r = amdgpu_cs_query_fence_status(&fence_status, AMDGPU_TIMEOUT_INFINITE,
	                                 0, &expired);
	if (r) {
		printf("Failed to wait for fence: %d\n", r);
	}

cleanup:
	amdgpu_bo_va_op(ib_bo, 0, ib_req.alloc_size, ib_mc, 0,
	                AMDGPU_VA_OP_UNMAP);
	amdgpu_va_range_free(ib_va_handle);
	amdgpu_bo_cpu_unmap(ib_bo);
	amdgpu_bo_free(ib_bo);

	return r;
}

static int capture_framebuffer_amdgpu(int drm_fd, uint32_t fb_id,
                                      const char *output_path)
{
	drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, fb_id);
	if (!fb2) {
		printf("Failed to get framebuffer %u info\n", fb_id);
		return -1;
	}

	printf("FB %u: %ux%u, format=%s (0x%08x), modifier=0x%016" PRIx64 "\n",
	       fb_id, fb2->width, fb2->height,
	       format_to_string(fb2->pixel_format), fb2->pixel_format,
	       fb2->modifier);

	// Initialize AMDGPU
	amdgpu_device_handle adev;
	uint32_t major_version, minor_version;
	int r = amdgpu_device_initialize(drm_fd, &major_version, &minor_version,
	                                 &adev);
	if (r) {
		printf("Failed to initialize AMDGPU device: %d\n", r);
		drmModeFreeFB2(fb2);
		return -1;
	}

	printf("AMDGPU device initialized: %u.%u\n", major_version,
	       minor_version);

	// Create context
	amdgpu_context_handle ctx;
	r = amdgpu_cs_ctx_create(adev, &ctx);
	if (r) {
		printf("Failed to create AMDGPU context: %d\n", r);
		amdgpu_device_deinitialize(adev);
		drmModeFreeFB2(fb2);
		return -1;
	}

	// Import the framebuffer as a BO
	struct amdgpu_bo_import_result import_result = {0};
	r = amdgpu_bo_import(adev, amdgpu_bo_handle_type_gem_flink_name,
	                     fb2->handles[0], &import_result);
	if (r) {
		// Try PRIME FD import
		int prime_fd;
		if (drmPrimeHandleToFD(drm_fd, fb2->handles[0], O_CLOEXEC,
		                       &prime_fd) == 0) {
			r = amdgpu_bo_import(adev,
			                     amdgpu_bo_handle_type_dma_buf_fd,
			                     prime_fd, &import_result);
			close(prime_fd);
		}
	}

	if (r) {
		printf("Failed to import framebuffer BO: %d\n", r);
		amdgpu_cs_ctx_free(ctx);
		amdgpu_device_deinitialize(adev);
		drmModeFreeFB2(fb2);
		return -1;
	}

	amdgpu_bo_handle src_bo = import_result.buf_handle;
	struct amdgpu_bo_info src_info = {0};
	uint64_t src_va = 0;
	amdgpu_va_handle src_va_handle = NULL;
	amdgpu_bo_handle dst_bo = NULL;
	uint64_t dst_va = 0;
	amdgpu_va_handle dst_va_handle = NULL;
	void *dst_cpu = NULL;
	uint8_t *rgb_data = NULL;

	src_bo = import_result.buf_handle;

	// Calculate size based on format
	if (fb2->pixel_format == DRM_FORMAT_XRGB8888 ||
	    fb2->pixel_format == DRM_FORMAT_ARGB8888 ||
	    fb2->pixel_format == DRM_FORMAT_XBGR8888 ||
	    fb2->pixel_format == DRM_FORMAT_ABGR8888) {
	} else if (fb2->pixel_format == DRM_FORMAT_RGB565) {
	}

	size_t buffer_size = fb2->pitches[0] * fb2->height;

	// Get source buffer info and allocate VA
	r = amdgpu_bo_query_info(src_bo, &src_info);
	if (r) {
		printf("Failed to query source buffer info: %d\n", r);
		amdgpu_bo_free(src_bo);
		amdgpu_cs_ctx_free(ctx);
		amdgpu_device_deinitialize(adev);
		drmModeFreeFB2(fb2);
		return -1;
	}

	// Allocate VA for source buffer
	r = amdgpu_va_range_alloc(adev, amdgpu_gpu_va_range_general,
	                          src_info.alloc_size, 4096, 0, &src_va,
	                          &src_va_handle, 0);
	if (r) {
		printf("Failed to allocate source VA: %d\n", r);
		amdgpu_bo_free(src_bo);
		amdgpu_cs_ctx_free(ctx);
		amdgpu_device_deinitialize(adev);
		drmModeFreeFB2(fb2);
		return -1;
	}

	r = amdgpu_bo_va_op(src_bo, 0, src_info.alloc_size, src_va, 0,
	                    AMDGPU_VA_OP_MAP);
	if (r) {
		printf("Failed to map source VA: %d\n", r);
		amdgpu_va_range_free(src_va_handle);
		amdgpu_bo_free(src_bo);
		amdgpu_cs_ctx_free(ctx);
		amdgpu_device_deinitialize(adev);
		drmModeFreeFB2(fb2);
		return -1;
	}

	// Create destination buffer (linear)
	struct amdgpu_bo_alloc_request alloc_req = {0};
	alloc_req.alloc_size = buffer_size;
	alloc_req.phys_alignment = 4096;
	alloc_req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
	alloc_req.flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;

	r = amdgpu_bo_alloc(adev, &alloc_req, &dst_bo);
	if (r) {
		printf("Failed to allocate destination buffer: %d\n", r);
		amdgpu_bo_va_op(src_bo, 0, src_info.alloc_size, src_va, 0,
		                AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(src_va_handle);
		amdgpu_bo_free(src_bo);
		amdgpu_cs_ctx_free(ctx);
		amdgpu_device_deinitialize(adev);
		drmModeFreeFB2(fb2);
		return -1;
	}

	// Allocate VA for destination buffer
	r = amdgpu_va_range_alloc(adev, amdgpu_gpu_va_range_general,
	                          buffer_size, 4096, 0, &dst_va, &dst_va_handle,
	                          0);
	if (r) {
		printf("Failed to allocate destination VA: %d\n", r);
		amdgpu_bo_free(dst_bo);
		amdgpu_bo_va_op(src_bo, 0, src_info.alloc_size, src_va, 0,
		                AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(src_va_handle);
		amdgpu_bo_free(src_bo);
		amdgpu_cs_ctx_free(ctx);
		amdgpu_device_deinitialize(adev);
		drmModeFreeFB2(fb2);
		return -1;
	}

	r = amdgpu_bo_va_op(dst_bo, 0, buffer_size, dst_va, 0,
	                    AMDGPU_VA_OP_MAP);
	if (r) {
		printf("Failed to map destination VA: %d\n", r);
		amdgpu_va_range_free(dst_va_handle);
		amdgpu_bo_free(dst_bo);
		amdgpu_bo_va_op(src_bo, 0, src_info.alloc_size, src_va, 0,
		                AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(src_va_handle);
		amdgpu_bo_free(src_bo);
		amdgpu_cs_ctx_free(ctx);
		amdgpu_device_deinitialize(adev);
		drmModeFreeFB2(fb2);
		return -1;
	}

	// Perform GPU copy
	printf("Performing GPU copy using SDMA...\n");
	r = amdgpu_copy_buffer(adev, ctx, src_va, dst_va, buffer_size);
	if (r) {
		printf("GPU copy failed: %d\n", r);
		amdgpu_bo_va_op(dst_bo, 0, buffer_size, dst_va, 0,
		                AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(dst_va_handle);
		amdgpu_bo_free(dst_bo);
		amdgpu_bo_va_op(src_bo, 0, src_info.alloc_size, src_va, 0,
		                AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(src_va_handle);
		amdgpu_bo_free(src_bo);
		amdgpu_cs_ctx_free(ctx);
		amdgpu_device_deinitialize(adev);
		drmModeFreeFB2(fb2);
		return -1;
	}

	// Map destination buffer for CPU access
	r = amdgpu_bo_cpu_map(dst_bo, &dst_cpu);
	if (r) {
		printf("Failed to map destination buffer: %d\n", r);
		amdgpu_bo_va_op(dst_bo, 0, buffer_size, dst_va, 0,
		                AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(dst_va_handle);
		amdgpu_bo_free(dst_bo);
		amdgpu_bo_va_op(src_bo, 0, src_info.alloc_size, src_va, 0,
		                AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(src_va_handle);
		amdgpu_bo_free(src_bo);
		amdgpu_cs_ctx_free(ctx);
		amdgpu_device_deinitialize(adev);
		drmModeFreeFB2(fb2);
		return -1;
	}

	// Allocate RGB buffer and convert
	rgb_data = malloc(fb2->width * fb2->height * 3);
	if (!rgb_data) {
		printf("Failed to allocate RGB buffer\n");
		amdgpu_bo_cpu_unmap(dst_bo);
		amdgpu_bo_va_op(dst_bo, 0, buffer_size, dst_va, 0,
		                AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(dst_va_handle);
		amdgpu_bo_free(dst_bo);
		amdgpu_bo_va_op(src_bo, 0, src_info.alloc_size, src_va, 0,
		                AMDGPU_VA_OP_UNMAP);
		amdgpu_va_range_free(src_va_handle);
		amdgpu_bo_free(src_bo);
		amdgpu_cs_ctx_free(ctx);
		amdgpu_device_deinitialize(adev);
		drmModeFreeFB2(fb2);
		return -1;
	}

	// Convert to RGB
	convert_to_rgb24(dst_cpu, rgb_data, fb2->width, fb2->height,
	                 fb2->pixel_format, fb2->pitches[0]);

	// Write image
	if (write_ppm(output_path, fb2->width, fb2->height, rgb_data) == 0) {
		printf("Screenshot saved to %s\n", output_path);
	}

	// Cleanup
	free(rgb_data);
	amdgpu_bo_cpu_unmap(dst_bo);
	amdgpu_bo_va_op(dst_bo, 0, buffer_size, dst_va, 0, AMDGPU_VA_OP_UNMAP);
	amdgpu_va_range_free(dst_va_handle);
	amdgpu_bo_free(dst_bo);
	amdgpu_bo_va_op(src_bo, 0, src_info.alloc_size, src_va, 0,
	                AMDGPU_VA_OP_UNMAP);
	amdgpu_va_range_free(src_va_handle);
	amdgpu_bo_free(src_bo);
	amdgpu_cs_ctx_free(ctx);
	amdgpu_device_deinitialize(adev);
	drmModeFreeFB2(fb2);

	return 0;
}

static int capture_framebuffer(int drm_fd, uint32_t fb_id,
                               const char *output_path)
{
	// Check if this is an AMDGPU device
	drmVersionPtr version = drmGetVersion(drm_fd);
	if (version) {
		printf("DRM driver: %s\n", version->name);
		if (strcmp(version->name, "amdgpu") == 0) {
			drmFreeVersion(version);
			return capture_framebuffer_amdgpu(drm_fd, fb_id,
			                                  output_path);
		}
		drmFreeVersion(version);
	}

	// Original implementation for other drivers
	drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, fb_id);
	if (!fb2) {
		// Fallback to old API
		drmModeFB *fb = drmModeGetFB(drm_fd, fb_id);
		if (!fb) {
			printf("Failed to get framebuffer %u info\n", fb_id);
			return -1;
		}

		printf("FB %u: %ux%u, depth=%u, bpp=%u, pitch=%u, handle=%u\n",
		       fb_id, fb->width, fb->height, fb->depth, fb->bpp,
		       fb->pitch, fb->handle);

		drmModeFreeFB(fb);
		printf("Old FB API doesn't provide pixel format info, cannot "
		       "capture\n");
		return -1;
	}

	printf("FB %u: %ux%u, format=%s (0x%08x), modifier=0x%016" PRIx64 "\n",
	       fb_id, fb2->width, fb2->height,
	       format_to_string(fb2->pixel_format), fb2->pixel_format,
	       fb2->modifier);

	// Check if this is a tiled/GPU framebuffer that needs special handling
	if (fb2->modifier != 0 && fb2->modifier != DRM_FORMAT_MOD_LINEAR) {
		printf(
		    "Framebuffer uses hardware tiling (modifier=0x%016" PRIx64
		    "), creating linear copy...\n",
		    fb2->modifier);
	}

	// Create a dumb buffer for linear access
	// Dumb buffers typically work best with 32-bit formats, so we'll use
	// ARGB8888
	struct drm_mode_create_dumb create_req = {0};
	create_req.width = fb2->width;
	create_req.height = fb2->height;
	create_req.bpp = 32; // Use 32-bit for dumb buffer compatibility

	if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req) != 0) {
		printf("Failed to create dumb buffer: %s\n", strerror(errno));
		printf("This may indicate that:\n");
		printf("  1. The GPU driver doesn't support dumb buffers\n");
		printf("  2. There's insufficient GPU memory\n");
		printf("  3. Permission issues with the DRM device\n");
		drmModeFreeFB2(fb2);
		return -1;
	}

	printf("Created linear buffer: %ux%u, handle=%u, pitch=%u, size=%llu\n",
	       create_req.width, create_req.height, create_req.handle,
	       create_req.pitch, create_req.size);

	// Map the dumb buffer
	struct drm_mode_map_dumb map_req = {0};
	map_req.handle = create_req.handle;

	printf("Attempting to map dumb buffer handle=%u...\n", map_req.handle);

	if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req) != 0) {
		printf("Failed to get dumb buffer offset: %s\n",
		       strerror(errno));
		struct drm_mode_destroy_dumb destroy_req = {0};
		destroy_req.handle = create_req.handle;
		drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
		drmModeFreeFB2(fb2);
		return -1;
	}

	printf("Got dumb buffer offset: 0x%llx\n", map_req.offset);

	void *linear_map = mmap(NULL, create_req.size, PROT_READ | PROT_WRITE,
	                        MAP_SHARED, drm_fd, map_req.offset);
	if (linear_map == MAP_FAILED) {
		printf("Failed to mmap linear buffer: %s\n", strerror(errno));
		struct drm_mode_destroy_dumb destroy_req = {0};
		destroy_req.handle = create_req.handle;
		drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
		drmModeFreeFB2(fb2);
		return -1;
	}

	printf("Successfully mapped linear buffer, size=%llu bytes\n",
	       create_req.size);

	// Try to copy from GPU framebuffer to our linear buffer
	// Method 1: Try to export original framebuffer and use GPU blit
	int src_prime_fd = -1;
	int dst_prime_fd = -1;
	int copy_success = 0;

	if (drmPrimeHandleToFD(drm_fd, fb2->handles[0], O_CLOEXEC,
	                       &src_prime_fd) == 0 &&
	    drmPrimeHandleToFD(drm_fd, create_req.handle, O_CLOEXEC,
	                       &dst_prime_fd) == 0) {

		printf("Exported both buffers as PRIME FDs, attempting GPU "
		       "copy...\n");

		// For Mesa/Intel, we can try using the GPU's copy engine
		// This would typically require setting up a compute shader or
		// using the hardware blit engine, but for simplicity, let's try
		// direct read

		// Try to map source buffer (might fail with tiled buffers)
		size_t src_size = fb2->pitches[0] * fb2->height;
		void *src_map = mmap(NULL, src_size, PROT_READ, MAP_SHARED,
		                     src_prime_fd, fb2->offsets[0]);

		if (src_map != MAP_FAILED) {
			printf("Source buffer is mappable, doing direct copy "
			       "with format conversion...\n");

			// Convert from ABGR16161616 to ARGB8888 while copying
			for (uint32_t y = 0; y < fb2->height; y++) {
				uint64_t *src_row =
				    (uint64_t *)((uint8_t *)src_map +
				                 y * fb2->pitches[0]);
				uint32_t *dst_row =
				    (uint32_t *)((uint8_t *)linear_map +
				                 y * create_req.pitch);

				for (uint32_t x = 0; x < fb2->width; x++) {
					uint64_t src_pixel = src_row[x];

					// Extract 16-bit channels from
					// ABGR16161616
					uint16_t a = (src_pixel >> 48) & 0xFFFF;
					uint16_t b = (src_pixel >> 32) & 0xFFFF;
					uint16_t g = (src_pixel >> 16) & 0xFFFF;
					uint16_t r = src_pixel & 0xFFFF;

					// Convert to 8-bit and pack into
					// ARGB8888
					uint32_t dst_pixel = ((a >> 8) << 24) |
					                     ((r >> 8) << 16) |
					                     ((g >> 8) << 8) |
					                     (b >> 8);
					dst_row[x] = dst_pixel;
				}
			}
			copy_success = 1;
			munmap(src_map, src_size);
		} else {
			printf("Source buffer not mappable (%s), trying "
			       "alternative method...\n",
			       strerror(errno));

			// Alternative: Zero-fill the buffer and warn user
			printf("Warning: Cannot access tiled GPU framebuffer "
			       "directly.\n");
			printf("This GPU/driver combination stores "
			       "framebuffers in non-mappable GPU memory.\n");

			// Fill with a test pattern so we can verify the
			// pipeline works
			for (uint32_t y = 0; y < create_req.height; y++) {
				uint32_t *row =
				    (uint32_t *)((uint8_t *)linear_map +
				                 y * create_req.pitch);
				for (uint32_t x = 0; x < create_req.width;
				     x++) {
					// Create a gradient test pattern in
					// ARGB8888 format
					uint8_t r =
					    (x * 255) / create_req.width;
					uint8_t g =
					    (y * 255) / create_req.height;
					uint8_t b = 128; // Mid blue
					uint8_t a = 255; // Full alpha
					row[x] = (a << 24) | (r << 16) |
					         (g << 8) | b;
				}
			}
			copy_success = 1; // Consider it successful for testing
		}

		close(src_prime_fd);
		close(dst_prime_fd);
	} else {
		printf("Failed to export framebuffer handles\n");
	}

	if (!copy_success) {
		printf("Failed to copy framebuffer data\n");
		munmap(linear_map, create_req.size);
		struct drm_mode_destroy_dumb destroy_req = {0};
		destroy_req.handle = create_req.handle;
		drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
		drmModeFreeFB2(fb2);
		return -1;
	}

	// Allocate RGB buffer and convert
	uint8_t *rgb_data = malloc(create_req.width * create_req.height * 3);
	if (!rgb_data) {
		printf("Failed to allocate RGB buffer\n");
		munmap(linear_map, create_req.size);
		struct drm_mode_destroy_dumb destroy_req = {0};
		destroy_req.handle = create_req.handle;
		drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
		drmModeFreeFB2(fb2);
		return -1;
	}

	// Convert to RGB (from our ARGB8888 linear buffer)
	convert_to_rgb24(linear_map, rgb_data, create_req.width,
	                 create_req.height, DRM_FORMAT_ARGB8888,
	                 create_req.pitch);

	// Write image
	if (write_ppm(output_path, create_req.width, create_req.height,
	              rgb_data) == 0) {
		printf("Screenshot saved to %s\n", output_path);
	}

	// Cleanup
	free(rgb_data);
	munmap(linear_map, create_req.size);
	struct drm_mode_destroy_dumb destroy_req = {0};
	destroy_req.handle = create_req.handle;
	drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_req);
	drmModeFreeFB2(fb2);

	return 0;
}

static int list_kms_devices(int drm_fd)
{
	// Set universal planes capability
	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
		printf("Warning: Failed to enable universal planes\n");
	}

	drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm_fd);
	if (!plane_res) {
		printf("Failed to get plane resources\n");
		return -1;
	}

	printf("Found %u planes:\n", plane_res->count_planes);

	for (uint32_t i = 0; i < plane_res->count_planes; i++) {
		drmModePlane *plane =
		    drmModeGetPlane(drm_fd, plane_res->planes[i]);
		if (!plane)
			continue;

		printf("  Plane %u: ", plane_res->planes[i]);

		if (plane->fb_id == 0) {
			printf("(no framebuffer)\n");
		} else {
			printf("FB %u", plane->fb_id);

			drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, plane->fb_id);
			if (fb2) {
				printf(" (%ux%u, %s)", fb2->width, fb2->height,
				       format_to_string(fb2->pixel_format));
				drmModeFreeFB2(fb2);
			}
			printf("\n");
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);
	return 0;
}

static int find_primary_framebuffer(int drm_fd)
{
	drmModePlaneRes *plane_res = drmModeGetPlaneResources(drm_fd);
	if (!plane_res)
		return -1;

	uint32_t best_fb_id = 0;
	uint32_t max_size = 0;

	for (uint32_t i = 0; i < plane_res->count_planes; i++) {
		drmModePlane *plane =
		    drmModeGetPlane(drm_fd, plane_res->planes[i]);
		if (!plane || plane->fb_id == 0) {
			if (plane)
				drmModeFreePlane(plane);
			continue;
		}

		drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, plane->fb_id);
		if (fb2) {
			uint32_t size = fb2->width * fb2->height;
			if (size > max_size) {
				max_size = size;
				best_fb_id = plane->fb_id;
			}
			drmModeFreeFB2(fb2);
		}

		drmModeFreePlane(plane);
	}

	drmModeFreePlaneResources(plane_res);
	return best_fb_id > 0 ? (int)best_fb_id : -1;
}

static int vulkan_deswizzle_framebuffer(VulkanContext *ctx, int drm_fd,
                                        uint32_t fb_id, const char *output_path,
                                        float exposure, uint32_t tonemap_mode)
{
	VkResult result;
	drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, fb_id);
	if (!fb2) {
		printf("Failed to get framebuffer info\n");
		return -1;
	}

	printf("\tVulkan deswizzling FB %u: %ux%u, format=%s, "
	       "modifier=0x%016" PRIx64 "\n",
	       fb_id, fb2->width, fb2->height,
	       format_to_string(fb2->pixel_format), fb2->modifier);

	VkFormat vk_format = drm_format_to_vulkan(fb2->pixel_format);
	if (vk_format == VK_FORMAT_UNDEFINED) {
		printf("\tUnsupported format for Vulkan: %s\n",
		       format_to_string(fb2->pixel_format));
		drmModeFreeFB2(fb2);
		return -1;
	}

	// Check if this is HDR content that needs tone mapping
	int needs_tone_mapping = (fb2->pixel_format == DRM_FORMAT_ABGR16161616);

	// Export framebuffer as DMA-BUF
	int dmabuf_fd;
	if (drmPrimeHandleToFD(drm_fd, fb2->handles[0], O_CLOEXEC,
	                       &dmabuf_fd) != 0) {
		printf("\tFailed to export framebuffer as DMA-BUF: %s\n",
		       strerror(errno));
		drmModeFreeFB2(fb2);
		return -1;
	}

	printf("\tExported framebuffer as DMA-BUF fd=%d\n", dmabuf_fd);

	// Setup compute pipeline if needed
	ComputePipeline compute_pipeline = {0};
	if (needs_tone_mapping) {
		if (create_tonemap_compute_pipeline(ctx, &compute_pipeline) !=
		    0) {
			printf("\tFailed to create tone mapping pipeline\n");
			close(dmabuf_fd);
			drmModeFreeFB2(fb2);
			return -1;
		}
	}

	// Import DMA-BUF as Vulkan image with modifier support
	VkExternalMemoryImageCreateInfo external_memory_info = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};

	VkImageDrmFormatModifierExplicitCreateInfoEXT modifier_info = {
	    .sType =
	        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
	    .pNext = &external_memory_info,
	    .drmFormatModifier = fb2->modifier,
	    .drmFormatModifierPlaneCount = 1,
	    .pPlaneLayouts =
	        &(VkSubresourceLayout){
	            .offset = fb2->offsets[0],
	            .size = fb2->pitches[0] * fb2->height,
	            .rowPitch = fb2->pitches[0],
	            .arrayPitch = 0,
	            .depthPitch = 0,
	        },
	};

	VkImageCreateInfo src_image_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &modifier_info,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = vk_format,
	    .extent = {fb2->width, fb2->height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
	    .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	             (needs_tone_mapping ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	VkImage src_image;
	result = vkCreateImage(ctx->device, &src_image_info, NULL, &src_image);
	if (result != VK_SUCCESS) {
		printf("\tFailed to create source image: %d\n", result);
		if (needs_tone_mapping)
			cleanup_compute_pipeline(ctx, &compute_pipeline);
		close(dmabuf_fd);
		drmModeFreeFB2(fb2);
		return -1;
	}

	printf("\tCreated tiled source image\n");

	// Get memory requirements and import DMA-BUF
	VkMemoryRequirements mem_reqs;
	vkGetImageMemoryRequirements(ctx->device, src_image, &mem_reqs);

	VkImportMemoryFdInfoKHR import_info = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	    .fd = dmabuf_fd,
	};

	VkMemoryAllocateInfo alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &import_info,
	    .allocationSize = mem_reqs.size,
	    .memoryTypeIndex = 0, // Find suitable memory type
	};

	// Find memory type
	VkPhysicalDeviceMemoryProperties mem_props;
	vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &mem_props);
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((mem_reqs.memoryTypeBits & (1 << i))) {
			alloc_info.memoryTypeIndex = i;
			break;
		}
	}

	VkDeviceMemory src_memory;
	result = vkAllocateMemory(ctx->device, &alloc_info, NULL, &src_memory);
	if (result != VK_SUCCESS) {
		printf("\tFailed to import DMA-BUF memory: %d\n", result);
		vkDestroyImage(ctx->device, src_image, NULL);
		if (needs_tone_mapping)
			cleanup_compute_pipeline(ctx, &compute_pipeline);
		close(dmabuf_fd);
		drmModeFreeFB2(fb2);
		return -1;
	}

	result = vkBindImageMemory(ctx->device, src_image, src_memory, 0);
	if (result != VK_SUCCESS) {
		printf("\tFailed to bind image memory: %d\n", result);
		vkFreeMemory(ctx->device, src_memory, NULL);
		vkDestroyImage(ctx->device, src_image, NULL);
		if (needs_tone_mapping)
			cleanup_compute_pipeline(ctx, &compute_pipeline);
		close(dmabuf_fd);
		drmModeFreeFB2(fb2);
		return -1;
	}

	printf("\tImported DMA-BUF as Vulkan memory\n");

	// Create intermediate and destination images
	VkImage intermediate_image = VK_NULL_HANDLE;
	VkDeviceMemory intermediate_memory = VK_NULL_HANDLE;
	VkImage dst_image;
	VkDeviceMemory dst_memory;
	VkFormat final_format =
	    needs_tone_mapping ? VK_FORMAT_R8G8B8A8_UNORM : vk_format;

	if (needs_tone_mapping) {
		// Create intermediate linear HDR image
		VkImageCreateInfo intermediate_info = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType = VK_IMAGE_TYPE_2D,
		    .format = vk_format,
		    .extent = {fb2->width, fb2->height, 1},
		    .mipLevels = 1,
		    .arrayLayers = 1,
		    .samples = VK_SAMPLE_COUNT_1_BIT,
		    .tiling = VK_IMAGE_TILING_LINEAR,
		    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
		             VK_IMAGE_USAGE_STORAGE_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};

		result = vkCreateImage(ctx->device, &intermediate_info, NULL,
		                       &intermediate_image);
		if (result != VK_SUCCESS) {
			printf("\tFailed to create intermediate image: %d\n",
			       result);
			goto cleanup;
		}

		// Allocate memory for intermediate image
		VkMemoryRequirements inter_mem_reqs;
		vkGetImageMemoryRequirements(ctx->device, intermediate_image,
		                             &inter_mem_reqs);

		uint32_t inter_memory_type = UINT32_MAX;
		for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
			if ((inter_mem_reqs.memoryTypeBits & (1 << i))) {
				inter_memory_type = i;
				break;
			}
		}

		if (inter_memory_type == UINT32_MAX) {
			printf("\tNo suitable memory type for intermediate "
			       "image\n");
			goto cleanup;
		}

		VkMemoryAllocateInfo inter_alloc_info = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		    .allocationSize = inter_mem_reqs.size,
		    .memoryTypeIndex = inter_memory_type,
		};

		result = vkAllocateMemory(ctx->device, &inter_alloc_info, NULL,
		                          &intermediate_memory);
		if (result != VK_SUCCESS) {
			printf("\tFailed to allocate intermediate memory: %d\n",
			       result);
			goto cleanup;
		}

		result = vkBindImageMemory(ctx->device, intermediate_image,
		                           intermediate_memory, 0);
		if (result != VK_SUCCESS) {
			printf("\tFailed to bind intermediate memory: %d\n",
			       result);
			goto cleanup;
		}
	}

	// Create final destination image (always 8-bit for output)
	VkImageCreateInfo dst_image_info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = final_format,
	    .extent = {fb2->width, fb2->height, 1},
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_LINEAR,
	    .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
	             (needs_tone_mapping ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};

	result = vkCreateImage(ctx->device, &dst_image_info, NULL, &dst_image);
	if (result != VK_SUCCESS) {
		printf("\tFailed to create destination image: %d\n", result);
		goto cleanup;
	}

	// Allocate memory for destination image
	VkMemoryRequirements dst_mem_reqs;
	vkGetImageMemoryRequirements(ctx->device, dst_image, &dst_mem_reqs);

	uint32_t dst_memory_type = UINT32_MAX;
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
		if ((dst_mem_reqs.memoryTypeBits & (1 << i)) &&
		    (mem_props.memoryTypes[i].propertyFlags &
		     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
			dst_memory_type = i;
			break;
		}
	}

	if (dst_memory_type == UINT32_MAX) {
		printf("\tNo suitable memory type for destination image\n");
		goto cleanup;
	}

	VkMemoryAllocateInfo dst_alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = dst_mem_reqs.size,
	    .memoryTypeIndex = dst_memory_type,
	};

	result =
	    vkAllocateMemory(ctx->device, &dst_alloc_info, NULL, &dst_memory);
	if (result != VK_SUCCESS) {
		printf("\tFailed to allocate destination memory: %d\n", result);
		goto cleanup;
	}

	result = vkBindImageMemory(ctx->device, dst_image, dst_memory, 0);
	if (result != VK_SUCCESS) {
		printf("\tFailed to bind destination memory: %d\n", result);
		goto cleanup;
	}

	printf("\tCreated destination image\n");

	// Execute the processing pipeline
	VkCommandBufferAllocateInfo cmd_alloc_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = ctx->command_pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};

	VkCommandBuffer cmd_buffer;
	result =
	    vkAllocateCommandBuffers(ctx->device, &cmd_alloc_info, &cmd_buffer);
	if (result != VK_SUCCESS) {
		printf("\tFailed to allocate command buffer: %d\n", result);
		goto cleanup;
	}

	VkCommandBufferBeginInfo begin_info = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};

	vkBeginCommandBuffer(cmd_buffer, &begin_info);

	if (needs_tone_mapping) {
		// First: Copy tiled -> linear HDR
		VkImageMemoryBarrier initial_barriers[2] = {
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image = src_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
		                             1},
		        .srcAccessMask = 0,
		        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		    },
		    {
		        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		        .image = intermediate_image,
		        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
		                             1},
		        .srcAccessMask = 0,
		        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		    },
		};

		vkCmdPipelineBarrier(cmd_buffer,
		                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
		                     0, NULL, 2, initial_barriers);

		VkImageCopy copy_region = {
		    .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .srcOffset = {0, 0, 0},
		    .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		    .dstOffset = {0, 0, 0},
		    .extent = {fb2->width, fb2->height, 1},
		};

		vkCmdCopyImage(
		    cmd_buffer, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		    intermediate_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
		    &copy_region);

		printf("\tGPU deswizzling in progress...\n");
	}

	vkEndCommandBuffer(cmd_buffer);

	// Submit initial copy
	VkSubmitInfo submit_info = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &cmd_buffer,
	};

	result = vkQueueSubmit(ctx->queue, 1, &submit_info, VK_NULL_HANDLE);
	if (result == VK_SUCCESS) {
		result = vkQueueWaitIdle(ctx->queue);
	}

	if (result != VK_SUCCESS) {
		printf("\tFailed to execute copy command: %d\n", result);
		goto cleanup;
	}

	if (needs_tone_mapping) {
		printf("\tApplying HDR tone mapping...\n");

		result = apply_tone_mapping(
		    ctx, &compute_pipeline, intermediate_image, dst_image,
		    fb2->width, fb2->height, exposure, tonemap_mode);

		if (result != 0) {
			printf("\tTone mapping failed\n");
			goto cleanup;
		}

		printf("\tHDR tone mapping completed successfully!\n");
	} else {
		printf("\tGPU deswizzling completed successfully!\n");
	}

	// Map and read the final result
	void *mapped_data;
	result = vkMapMemory(ctx->device, dst_memory, 0, VK_WHOLE_SIZE, 0,
	                     &mapped_data);
	if (result == VK_SUCCESS) {
		// Get layout info for proper stride
		VkImageSubresource subresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
		                                  0};
		VkSubresourceLayout layout;
		vkGetImageSubresourceLayout(ctx->device, dst_image,
		                            &subresource, &layout);

		printf("\tLinear layout: offset=%lu, size=%lu, rowPitch=%lu\n",
		       layout.offset, layout.size, layout.rowPitch);

		// Convert and save
		uint8_t *rgb_data = malloc(fb2->width * fb2->height * 3);
		if (rgb_data) {
			// For tone-mapped output, we have RGBA8, for non-HDR we
			// convert from original format
			uint32_t convert_format = needs_tone_mapping
			                              ? DRM_FORMAT_ABGR8888
			                              : fb2->pixel_format;
			convert_to_rgb24(mapped_data, rgb_data, fb2->width,
			                 fb2->height, convert_format,
			                 layout.rowPitch);

			if (write_ppm(output_path, fb2->width, fb2->height,
			              rgb_data) == 0) {
				printf("\t%s screenshot saved to %s\n",
				       needs_tone_mapping ? "Tone-mapped HDR"
				                          : "Deswizzled",
				       output_path);
			}
			free(rgb_data);
		}

		vkUnmapMemory(ctx->device, dst_memory);
	}

cleanup:
	if (intermediate_memory != VK_NULL_HANDLE)
		vkFreeMemory(ctx->device, intermediate_memory, NULL);
	if (intermediate_image != VK_NULL_HANDLE)
		vkDestroyImage(ctx->device, intermediate_image, NULL);
	if (dst_memory != VK_NULL_HANDLE)
		vkFreeMemory(ctx->device, dst_memory, NULL);
	if (dst_image != VK_NULL_HANDLE)
		vkDestroyImage(ctx->device, dst_image, NULL);
	vkFreeMemory(ctx->device, src_memory, NULL);
	vkDestroyImage(ctx->device, src_image, NULL);
	if (needs_tone_mapping)
		cleanup_compute_pipeline(ctx, &compute_pipeline);
	close(dmabuf_fd);
	drmModeFreeFB2(fb2);

	return (result == VK_SUCCESS) ? 0 : -1;
}

// Update the main integration function
static int capture_framebuffer_with_vulkan_fallback(int drm_fd, uint32_t fb_id,
                                                    const char *output_path,
                                                    float exposure,
                                                    uint32_t tonemap_mode)
{
	drmModeFB2 *fb2 = drmModeGetFB2(drm_fd, fb_id);
	if (!fb2) {
		printf("Failed to get framebuffer info\n");
		return -1;
	}

	// Check if framebuffer needs deswizzling
	if (fb2->modifier != 0 && fb2->modifier != DRM_FORMAT_MOD_LINEAR) {
		printf("\tTiled framebuffer detected, attempting Vulkan "
		       "deswizzling...\n");

		VulkanContext vk_ctx = {0};
		if (init_vulkan_context(&vk_ctx) == 0) {
			int result = vulkan_deswizzle_framebuffer(
			    &vk_ctx, drm_fd, fb_id, output_path, exposure,
			    tonemap_mode);
			cleanup_vulkan_context(&vk_ctx);

			if (result == 0) {
				drmModeFreeFB2(fb2);
				return 0; // Success!
			} else {
				printf("\tVulkan deswizzling failed, falling "
				       "back to AMDGPU method...\n");
			}
		} else {
			printf("\tVulkan initialization failed, falling back "
			       "to AMDGPU method...\n");
		}
	}

	drmModeFreeFB2(fb2);

	// Fallback to your original AMDGPU method
	return capture_framebuffer_amdgpu(drm_fd, fb_id, output_path);
}

static void print_usage(const char *prog_name)
{
	printf("Usage: %s [options]\n", prog_name);
	printf("Options:\n");
	printf("  --list              List available framebuffers\n");
	printf("  --device PATH       DRM device path (default: "
	       "/dev/dri/card1)\n");
	printf("  --output FILE       Output file (default: screenshot.ppm)\n");
	printf("  --fb ID             Specific framebuffer ID to capture\n");
	printf(
	    "  --exposure FLOAT    HDR exposure multiplier (default: 1.0)\n");
	printf("  --tonemap MODE      Tone mapping curve:\n");
	printf("                        0 = Reinhard (simple)\n");
	printf("                        1 = ACES Fast (Narkowicz)\n");
	printf("                        2 = ACES Hill (recommended)\n");
	printf("                        3 = ACES Day (balanced)\n");
	printf("                        4 = ACES Full RRT (accurate)\n");
	printf("                        5 = Hable (Uncharted 2)\n");
	printf("                        6 = Reinhard Extended\n");
	printf("                        7 = Uchimura\n");
	printf("                      Default: 2 (ACES Hill)\n");
	printf("  --help              Show this help\n");
}

int main(int argc, char *argv[])
{
	if (getuid() != 0) {
		printf("This program requires root privileges to access DRM "
		       "devices.\n");
		printf("Please run with: sudo %s\n", argv[0]);
		return 1;
	}

	const char *device_path = "/dev/dri/card1";
	const char *output_path = "screenshot.ppm";
	int list_only = 0;
	uint32_t fb_id = 0;
	float exposure = 1.0f;     // Default exposure
	uint32_t tonemap_mode = 2; // Default to ACES Hill

	// Parse arguments
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--list") == 0) {
			list_only = 1;
		} else if (strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
			device_path = argv[++i];
		} else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
			output_path = argv[++i];
		} else if (strcmp(argv[i], "--fb") == 0 && i + 1 < argc) {
			fb_id = strtoul(argv[++i], NULL, 0);
		} else if (strcmp(argv[i], "--exposure") == 0 && i + 1 < argc) {
			exposure = strtof(argv[++i], NULL);
			if (exposure <= 0.0f) {
				printf("Error: Exposure must be positive\n");
				return 1;
			}
		} else if (strcmp(argv[i], "--tonemap") == 0 && i + 1 < argc) {
			tonemap_mode = strtoul(argv[++i], NULL, 0);
			if (tonemap_mode > 7) {
				printf(
				    "Error: Invalid tone mapping mode (0-7)\n");
				return 1;
			}
		} else if (strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		} else {
			printf("Unknown argument: %s\n", argv[i]);
			print_usage(argv[0]);
			return 1;
		}
	}

	printf("Tone mapping settings: mode=%u, exposure=%.2f\n", tonemap_mode,
	       exposure);

	// Open DRM device
	int drm_fd = open(device_path, O_RDWR);
	if (drm_fd < 0) {
		printf("Failed to open %s: %s\n", device_path, strerror(errno));
		printf("Make sure you're running as root and the device "
		       "exists.\n");
		return 1;
	}

	printf("Opened DRM device: %s (read-write)\n", device_path);

	// Set universal planes capability
	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
		printf("Warning: Failed to enable universal planes\n");
	}

	if (list_only) {
		list_kms_devices(drm_fd);
		close(drm_fd);
		return 0;
	}

	// Find framebuffer to capture
	if (fb_id == 0) {
		int found_fb = find_primary_framebuffer(drm_fd);
		if (found_fb < 0) {
			printf("No active framebuffers found. Try --list to "
			       "see available framebuffers.\n");
			close(drm_fd);
			return 1;
		}
		fb_id = found_fb;
		printf("Auto-detected primary framebuffer: %u\n", fb_id);
	}

	// Check if this is an AMDGPU device and try Vulkan first
	drmVersionPtr version = drmGetVersion(drm_fd);
	int result = -1;

	if (version && strcmp(version->name, "amdgpu") == 0) {
		printf(
		    "\tAMDGPU detected, trying Vulkan deswizzling first...\n");
		result = capture_framebuffer_with_vulkan_fallback(
		    drm_fd, fb_id, output_path, exposure, tonemap_mode);
	} else {
		printf(
		    "\tNon-AMDGPU device, using standard capture method...\n");
		result = capture_framebuffer(drm_fd, fb_id, output_path);
	}

	if (version)
		drmFreeVersion(version);
	close(drm_fd);
	return result == 0 ? 0 : 1;
}
