/* M2: draw a triangle through a real graphics pipeline, render to an offscreen
 * R8G8B8A8 image, read it back and check pixels. Proves the shader compiler,
 * the graphics pipeline and the tiler path on kbase. No loader, no WSI.
 *
 * Build (NDK shim):
 *   aarch64-linux-android29-clang -O1 -DVK_NO_PROTOTYPES m2_vk_triangle.c -o m2 -ldl
 * Run (bundled bionic):
 *   LD_LIBRARY_PATH=/data/local/tmp/pb ./linker64 ./m2 ./libvulkan_panfrost.so
 *
 * PASS = centre pixel is the triangle colour (red) and a corner pixel is the
 * clear colour (black). Clear = (0,0,0,255), triangle = (255,0,0,255).
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "tri_vert.spv.h" /* const uint32_t tri_vert[] */
#include "tri_frag.spv.h" /* const uint32_t tri_frag[] */

#define W 64
#define H 64

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;
#define IFN(n) PFN_##n n = (PFN_##n)gipa(inst, #n); if (!n) die("missing " #n)
#define DFN(n) PFN_##n n = (PFN_##n)gdpa(dev, #n); if (!n) die("missing " #n)

static void die(const char *m){ fprintf(stderr, "FAIL: %s\n", m); exit(1); }
static void ck(VkResult r, const char *m){ if (r){ fprintf(stderr, "FAIL: %s -> %d\n", m, r); exit(1); } }

static uint32_t find_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want){
   for (uint32_t i=0;i<mp->memoryTypeCount;i++)
      if ((bits&(1u<<i)) && (mp->memoryTypes[i].propertyFlags & want)==want) return i;
   die("no memory type"); return 0;
}

int main(int argc, char **argv){
   const char *icd = argc>1 ? argv[1] : "libvulkan_panfrost.so";
   void *h = dlopen(icd, RTLD_NOW);
   if (!h){ fprintf(stderr,"FAIL: dlopen %s: %s\n", icd, dlerror()); return 1; }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(h,"vk_icdGetInstanceProcAddr");
   if (!gipa) die("no vk_icdGetInstanceProcAddr");

   PFN_vkCreateInstance vkCreateInstance=(PFN_vkCreateInstance)gipa(NULL,"vkCreateInstance");
   if (!vkCreateInstance) die("vkCreateInstance");
   VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="m2",.apiVersion=VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai};
   ck(vkCreateInstance(&ici,NULL,&inst),"vkCreateInstance");

   IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties);
   IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkGetPhysicalDeviceMemoryProperties);
   IFN(vkCreateDevice); IFN(vkGetDeviceProcAddr);
   gdpa=vkGetDeviceProcAddr;

   uint32_t n=1; VkPhysicalDevice pd;
   VkResult r=vkEnumeratePhysicalDevices(inst,&n,&pd);
   if ((r&&r!=VK_INCOMPLETE)||n==0) die("no physical device");
   VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd,&props);
   printf("device: %s  api %u.%u.%u\n", props.deviceName,
          VK_VERSION_MAJOR(props.apiVersion),VK_VERSION_MINOR(props.apiVersion),VK_VERSION_PATCH(props.apiVersion));

   uint32_t qn=0; vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,NULL);
   VkQueueFamilyProperties *qf=calloc(qn,sizeof *qf); vkGetPhysicalDeviceQueueFamilyProperties(pd,&qn,qf);
   uint32_t qi=qn;
   for (uint32_t i=0;i<qn;i++) if (qf[i].queueFlags&VK_QUEUE_GRAPHICS_BIT){ qi=i; break; }
   if (qi==qn) die("no graphics queue");

   float prio=1.0f;
   VkDeviceQueueCreateInfo qci={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=qi,.queueCount=1,.pQueuePriorities=&prio};
   VkDeviceCreateInfo dci={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qci};
   ck(vkCreateDevice(pd,&dci,NULL,&dev),"vkCreateDevice");

   DFN(vkGetDeviceQueue); DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory);
   DFN(vkBindImageMemory); DFN(vkCreateImageView); DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements);
   DFN(vkBindBufferMemory); DFN(vkCreateRenderPass); DFN(vkCreateFramebuffer); DFN(vkCreateShaderModule);
   DFN(vkCreatePipelineLayout); DFN(vkCreateGraphicsPipelines); DFN(vkCreateCommandPool);
   DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkCmdBeginRenderPass); DFN(vkCmdBindPipeline);
   DFN(vkCmdDraw); DFN(vkCmdEndRenderPass); DFN(vkCmdPipelineBarrier); DFN(vkCmdCopyImageToBuffer);
   DFN(vkEndCommandBuffer); DFN(vkQueueSubmit); DFN(vkQueueWaitIdle); DFN(vkDeviceWaitIdle); DFN(vkMapMemory);

   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd,&mp);
   VkQueue q; vkGetDeviceQueue(dev,qi,0,&q);

   /* color image */
   VkImageCreateInfo ic={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,
      .format=VK_FORMAT_R8G8B8A8_UNORM,.extent={W,H,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,
      .tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode=VK_SHARING_MODE_EXCLUSIVE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
   VkImage img; ck(vkCreateImage(dev,&ic,NULL,&img),"vkCreateImage");
   VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev,img,&mr);
   VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,
      .memoryTypeIndex=find_mem(&mp,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
   VkDeviceMemory imem; ck(vkAllocateMemory(dev,&mai,NULL,&imem),"alloc image mem");
   ck(vkBindImageMemory(dev,img,imem,0),"bind image");

   VkImageViewCreateInfo ivci={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=img,.viewType=VK_IMAGE_VIEW_TYPE_2D,
      .format=VK_FORMAT_R8G8B8A8_UNORM,.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
   VkImageView view; ck(vkCreateImageView(dev,&ivci,NULL,&view),"vkCreateImageView");

   /* render pass: clear -> draw -> store, final layout transfer-src */
   VkAttachmentDescription att={.format=VK_FORMAT_R8G8B8A8_UNORM,.samples=VK_SAMPLE_COUNT_1_BIT,
      .loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,
      .stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
      .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
   VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ref};
   VkSubpassDependency deps[2]={
      {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,
       .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
       .srcAccessMask=0,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
      {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,
       .srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT,
       .srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT}};
   VkRenderPassCreateInfo rpci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&att,
      .subpassCount=1,.pSubpasses=&sub,.dependencyCount=2,.pDependencies=deps};
   VkRenderPass rp; ck(vkCreateRenderPass(dev,&rpci,NULL,&rp),"vkCreateRenderPass");

   VkFramebufferCreateInfo fbci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=rp,.attachmentCount=1,
      .pAttachments=&view,.width=W,.height=H,.layers=1};
   VkFramebuffer fb; ck(vkCreateFramebuffer(dev,&fbci,NULL,&fb),"vkCreateFramebuffer");

   /* shaders */
   VkShaderModuleCreateInfo vsci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(tri_vert),.pCode=tri_vert};
   VkShaderModuleCreateInfo fsci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=sizeof(tri_frag),.pCode=tri_frag};
   VkShaderModule vs,fs; ck(vkCreateShaderModule(dev,&vsci,NULL,&vs),"vs"); ck(vkCreateShaderModule(dev,&fsci,NULL,&fs),"fs");

   VkPipelineLayoutCreateInfo plci={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout pl; ck(vkCreatePipelineLayout(dev,&plci,NULL,&pl),"pipeline layout");

   VkPipelineShaderStageCreateInfo stages[2]={
      {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=vs,.pName="main"},
      {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fs,.pName="main"}};
   VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo iasm={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp={0,0,W,H,0,1}; VkRect2D sc={{0,0},{W,H}};
   VkPipelineViewportStateCreateInfo vps={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.pViewports=&vp,.scissorCount=1,.pScissors=&sc};
   VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1.0f};
   VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba={.blendEnable=VK_FALSE,.colorWriteMask=0xf};
   VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&cba};
   VkGraphicsPipelineCreateInfo gpci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
      .pVertexInputState=&vi,.pInputAssemblyState=&iasm,.pViewportState=&vps,.pRasterizationState=&rs,
      .pMultisampleState=&ms,.pColorBlendState=&cb,.layout=pl,.renderPass=rp,.subpass=0};
   VkPipeline pipe; ck(vkCreateGraphicsPipelines(dev,VK_NULL_HANDLE,1,&gpci,NULL,&pipe),"vkCreateGraphicsPipelines");

   /* readback buffer */
   VkDeviceSize bsz=W*H*4;
   VkBufferCreateInfo bc={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=bsz,.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT,.sharingMode=VK_SHARING_MODE_EXCLUSIVE};
   VkBuffer buf; ck(vkCreateBuffer(dev,&bc,NULL,&buf),"vkCreateBuffer");
   vkGetBufferMemoryRequirements(dev,buf,&mr);
   mai.allocationSize=mr.size;
   mai.memoryTypeIndex=find_mem(&mp,mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VkDeviceMemory bmem; ck(vkAllocateMemory(dev,&mai,NULL,&bmem),"alloc buf mem");
   ck(vkBindBufferMemory(dev,buf,bmem,0),"bind buffer");

   VkCommandPoolCreateInfo pci={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=qi};
   VkCommandPool pool; ck(vkCreateCommandPool(dev,&pci,NULL,&pool),"cmd pool");
   VkCommandBufferAllocateInfo cai={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
   VkCommandBuffer cbuf; ck(vkAllocateCommandBuffers(dev,&cai,&cbuf),"alloc cmdbuf");

   VkCommandBufferBeginInfo bi={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
   ck(vkBeginCommandBuffer(cbuf,&bi),"begin");
   VkClearValue clear={.color={.float32={0,0,0,1}}};
   VkRenderPassBeginInfo rpbi={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=rp,.framebuffer=fb,
      .renderArea={{0,0},{W,H}},.clearValueCount=1,.pClearValues=&clear};
   vkCmdBeginRenderPass(cbuf,&rpbi,VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cbuf,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
   vkCmdDraw(cbuf,3,1,0,0);
   vkCmdEndRenderPass(cbuf);

   VkBufferImageCopy reg={.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.imageExtent={W,H,1}};
   vkCmdCopyImageToBuffer(cbuf,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buf,1,&reg);
   VkBufferMemoryBarrier b3={.sType=VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask=VK_ACCESS_HOST_READ_BIT,.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.buffer=buf,.size=VK_WHOLE_SIZE};
   vkCmdPipelineBarrier(cbuf,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,NULL,1,&b3,0,NULL);
   ck(vkEndCommandBuffer(cbuf),"end");

   VkSubmitInfo si={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cbuf};
   ck(vkQueueSubmit(q,1,&si,VK_NULL_HANDLE),"vkQueueSubmit");
   ck(vkQueueWaitIdle(q),"vkQueueWaitIdle");
   ck(vkDeviceWaitIdle(dev),"vkDeviceWaitIdle");

   unsigned char *px; ck(vkMapMemory(dev,bmem,0,bsz,0,(void**)&px),"map");
   unsigned char *ctr=&px[(32*W+32)*4];
   unsigned char *cor=&px[(2*W+2)*4];
   printf("centre(32,32)=%u %u %u %u  corner(2,2)=%u %u %u %u\n",
          ctr[0],ctr[1],ctr[2],ctr[3], cor[0],cor[1],cor[2],cor[3]);
   int ctr_red = (abs(ctr[0]-255)<=2 && ctr[1]<=2 && ctr[2]<=2 && abs(ctr[3]-255)<=2);
   int cor_black = (cor[0]<=2 && cor[1]<=2 && cor[2]<=2 && abs(cor[3]-255)<=2);
   if (ctr_red && cor_black){ printf("M2 PASS: triangle rendered (centre red, corner clear)\n"); return 0; }
   printf("M2 FAIL: centre_red=%d corner_black=%d\n", ctr_red, cor_black);
   return 2;
}
