/* csgprobe: print the CSF global interface (CSG slots, CSIs per group) via
 * KBASE_IOCTL_CS_GET_GLB_IFACE, using the same device-open and handshake
 * sequence as panvk's kbase_kmod.c (kbase_kmod_dev_create + kbase_query_csif_info).
 * Definitions copied from mesa include/drm-uapi/mali_kbase_ioctl.h,
 * src/panfrost/lib/kmod/kbase_uapi.h, kbase_csf_uapi.h, mali_base_csf_kernel.h. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <linux/types.h>

#define KBASE_IOCTL_TYPE 0x80

struct kbase_ioctl_version_check { __u16 major; __u16 minor; };
#define KBASE_IOCTL_VERSION_CHECK_JM \
   _IOWR(KBASE_IOCTL_TYPE, 0, struct kbase_ioctl_version_check)
#define KBASE_IOCTL_VERSION_CHECK_CSF \
   _IOWR(KBASE_IOCTL_TYPE, 52, struct kbase_ioctl_version_check)

struct kbase_ioctl_set_flags { __u32 create_flags; };
#define KBASE_IOCTL_SET_FLAGS \
   _IOW(KBASE_IOCTL_TYPE, 1, struct kbase_ioctl_set_flags)

struct kbase_ioctl_get_gpuprops { __u64 buffer; __u32 size; __u32 flags; };
#define KBASE_IOCTL_GET_GPUPROPS \
   _IOW(KBASE_IOCTL_TYPE, 3, struct kbase_ioctl_get_gpuprops)

struct kbase_ioctl_mem_jit_init {
   __u64 va_pages;
   __u8 max_allocations;
   __u8 trim_level;
   __u8 group_id;
   __u8 padding[5];
   __u64 phys_pages;
};
#define KBASE_IOCTL_MEM_JIT_INIT \
   _IOW(KBASE_IOCTL_TYPE, 14, struct kbase_ioctl_mem_jit_init)

struct kbase_ioctl_mem_exec_init { __u64 va_pages; };
#define KBASE_IOCTL_MEM_EXEC_INIT \
   _IOW(KBASE_IOCTL_TYPE, 38, struct kbase_ioctl_mem_exec_init)

union kbase_ioctl_cs_get_glb_iface {
   struct {
      __u32 max_group_num;
      __u32 max_total_stream_num;
      __u64 groups_ptr;
      __u64 streams_ptr;
   } in;
   struct {
      __u32 glb_version;
      __u32 features;
      __u32 group_num;
      __u32 prfcnt_size;
      __u32 total_stream_num;
      __u32 instr_features;
   } out;
};
#define KBASE_IOCTL_CS_GET_GLB_IFACE \
   _IOWR(KBASE_IOCTL_TYPE, 51, union kbase_ioctl_cs_get_glb_iface)

struct basep_cs_stream_control { __u32 features; __u32 padding; };
struct basep_cs_group_control {
   __u32 features;
   __u32 stream_num;
   __u32 suspend_size;
   __u32 padding;
};

_Static_assert(sizeof(union kbase_ioctl_cs_get_glb_iface) == 24, "glb_iface size");
_Static_assert(sizeof(struct kbase_ioctl_mem_jit_init) == 24, "jit_init size");
_Static_assert(sizeof(struct basep_cs_group_control) == 16, "group size");
_Static_assert(sizeof(struct basep_cs_stream_control) == 8, "stream size");

int main(int argc, char **argv)
{
   const char *path = argc > 1 ? argv[1] : "/dev/mali0";

   /* panvk_physical_device.c create_kmod_dev_kbase */
   int fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) { printf("open %s: %s\n", path, strerror(errno)); return 1; }

   /* 1. version handshake, CSF first then JM, input 0.0 */
   struct kbase_ioctl_version_check ver = { 0 };
   if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK_CSF, &ver) == 0) {
      printf("VERSION_CHECK_CSF ok: uAPI %u.%u\n", ver.major, ver.minor);
   } else {
      int e = errno;
      memset(&ver, 0, sizeof(ver));
      if (ioctl(fd, KBASE_IOCTL_VERSION_CHECK_JM, &ver) == 0)
         printf("CSF check failed (%s); JM driver %u.%u, not CSF\n",
                strerror(e), ver.major, ver.minor);
      else
         printf("version handshake rejected: CSF %s, JM %s\n",
                strerror(e), strerror(errno));
      return 1;
   }

   /* 2. SET_FLAGS 0 (creates the kernel context) */
   struct kbase_ioctl_set_flags set_flags = { .create_flags = 0 };
   if (ioctl(fd, KBASE_IOCTL_SET_FLAGS, &set_flags)) {
      printf("SET_FLAGS: %s\n", strerror(errno));
      return 1;
   }

   /* 3. tracking page: BASE_MEM_MAP_TRACKING_HANDLE = 3 << PAGE_SHIFT */
   off_t tracking_off = 3 * (off_t)sysconf(_SC_PAGESIZE);
   void *tracking = mmap(NULL, 4096, PROT_NONE, MAP_SHARED, fd, tracking_off);
   if (tracking == MAP_FAILED) {
      printf("mmap tracking page: %s\n", strerror(errno));
      return 1;
   }

   /* 4. GET_GPUPROPS probe then fill (driver aborts on failure; we only warn,
    * since the props are not needed for the GLB iface query) */
   struct kbase_ioctl_get_gpuprops props = { 0 };
   int psize = ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &props);
   if (psize > 0) {
      void *pbuf = malloc(psize);
      props.buffer = (uintptr_t)pbuf;
      props.size = (uint32_t)psize;
      if (pbuf && ioctl(fd, KBASE_IOCTL_GET_GPUPROPS, &props) >= 0)
         printf("GET_GPUPROPS ok: %d bytes\n", psize);
      else
         printf("warn: GET_GPUPROPS fill: %s\n", strerror(errno));
      free(pbuf);
   } else {
      printf("warn: GET_GPUPROPS probe returned %d: %s\n", psize, strerror(errno));
   }

   /* 5. JIT_INIT (non-fatal in driver) */
   struct kbase_ioctl_mem_jit_init jit_init = {
      .va_pages = 1ull << 25,
      .max_allocations = 255,
      .phys_pages = 1ull << 25,
   };
   if (ioctl(fd, KBASE_IOCTL_MEM_JIT_INIT, &jit_init))
      printf("warn: MEM_JIT_INIT: %s\n", strerror(errno));

   /* 6. EXEC_INIT (non-fatal in driver) */
   struct kbase_ioctl_mem_exec_init exec_init = { .va_pages = 0x100000 };
   if (ioctl(fd, KBASE_IOCTL_MEM_EXEC_INIT, &exec_init))
      printf("warn: MEM_EXEC_INIT: %s\n", strerror(errno));

   /* 7. CS_GET_GLB_IFACE probe with zero sizes */
   union kbase_ioctl_cs_get_glb_iface req = { 0 };
   if (ioctl(fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &req) < 0) {
      printf("CS_GET_GLB_IFACE (probe): %s\n", strerror(errno));
      return 1;
   }
   uint32_t group_num = req.out.group_num;
   uint32_t total_stream_num = req.out.total_stream_num;
   printf("probe: glb_version 0x%08x (%u.%u.%u) features 0x%08x group_num %u "
          "total_stream_num %u prfcnt_size 0x%x instr_features 0x%x\n",
          req.out.glb_version, req.out.glb_version >> 24,
          (req.out.glb_version >> 16) & 0xff, req.out.glb_version & 0xffff,
          req.out.features, group_num, total_stream_num,
          req.out.prfcnt_size, req.out.instr_features);
   if (!group_num || !total_stream_num) {
      printf("no queue groups/streams reported\n");
      return 1;
   }

   /* 8. fill */
   struct basep_cs_group_control *groups = calloc(group_num, sizeof(*groups));
   struct basep_cs_stream_control *streams =
      calloc(total_stream_num, sizeof(*streams));
   if (!groups || !streams) { printf("calloc failed\n"); return 1; }

   req.in.max_group_num = group_num;
   req.in.max_total_stream_num = total_stream_num;
   req.in.groups_ptr = (uintptr_t)groups;
   req.in.streams_ptr = (uintptr_t)streams;
   if (ioctl(fd, KBASE_IOCTL_CS_GET_GLB_IFACE, &req) < 0) {
      printf("CS_GET_GLB_IFACE (fill): %s\n", strerror(errno));
      return 1;
   }

   printf("\nCSG slots (group_num): %u\n", req.out.group_num);
   printf("total CSIs (total_stream_num): %u\n", req.out.total_stream_num);
   printf("glb_version 0x%08x  features 0x%08x\n",
          req.out.glb_version, req.out.features);

   uint32_t s = 0;
   for (uint32_t i = 0; i < group_num; i++) {
      printf("group %2u: stream_num %u  suspend_size %u (0x%x)  features 0x%08x\n",
             i, groups[i].stream_num, groups[i].suspend_size,
             groups[i].suspend_size, groups[i].features);
      for (uint32_t j = 0; j < groups[i].stream_num && s < total_stream_num; j++, s++)
         printf("    stream %u: features 0x%08x\n", j, streams[s].features);
   }

   /* same derivation as kbase_kmod.c:453-469 */
   uint32_t sf = streams[0].features;
   uint32_t sb = (sf >> 8) & 0xff;
   uint32_t regs = (sf & 0xff) + 1;
   printf("\nstreams[0].features 0x%08x\n", sf);
   printf("cs_reg_count (WORK_REGS [7:0] + 1): %u\n", regs);
   printf("scoreboard_slot_count raw [15:8]: %u  (driver uses %u)\n",
          sb, (sb - 1) < 16 ? sb : 8);
   printf("cs_slot_count used by driver (groups[0].stream_num): %u\n",
          groups[0].stream_num);

   free(groups);
   free(streams);
   munmap(tracking, 4096);
   close(fd);
   return 0;
}
