/* SPDX-License-Identifier: MIT; compile-time proof that the SDK and kernel
 * declare the same records. Built for the host and for ARM32 by test_abi.py. */
#include "abi.h"
#include <sandbox/abi.h>
#define SAME(k,s,f) _Static_assert(offsetof(struct k,f)==offsetof(s,f)&&sizeof(((struct k *)0)->f)==sizeof(((s *)0)->f),#s "." #f)
#define SIZE(k,s) _Static_assert(sizeof(struct k)==sizeof(s),#s)
SIZE(k_startup_ext,sbx_startup_ext); SAME(k_startup_ext,sbx_startup_ext,prefix); SAME(k_startup_ext,sbx_startup_ext,app_generation);
SIZE(k_app_arena,sbx_app_arena_rec); SAME(k_app_arena,sbx_app_arena_rec,h); SAME(k_app_arena,sbx_app_arena_rec,owner_generation); SAME(k_app_arena,sbx_app_arena_rec,requested_bytes); SAME(k_app_arena,sbx_app_arena_rec,address); SAME(k_app_arena,sbx_app_arena_rec,actual_bytes);
_Static_assert(SBX_CALL_APP_ARENA==21 && SBX_EOVERFLOW==75,"arena constants");
SIZE(k_header,sbx_header); SAME(k_header,sbx_header,size); SAME(k_header,sbx_header,flags); SAME(k_header,sbx_header,reserved0); SAME(k_header,sbx_header,reserved1);
SIZE(k_open,sbx_open_rec); SAME(k_open,sbx_open_rec,path); SAME(k_open,sbx_open_rec,path_bytes); SAME(k_open,sbx_open_rec,flags); SAME(k_open,sbx_open_rec,mode); SAME(k_open,sbx_open_rec,fd);
SIZE(k_io,sbx_io_rec); SAME(k_io,sbx_io_rec,fd); SAME(k_io,sbx_io_rec,buffer); SAME(k_io,sbx_io_rec,count); SAME(k_io,sbx_io_rec,transferred);
SIZE(k_close,sbx_close_rec); SAME(k_close,sbx_close_rec,fd);
SIZE(k_rename,sbx_rename_rec); SAME(k_rename,sbx_rename_rec,from); SAME(k_rename,sbx_rename_rec,from_bytes); SAME(k_rename,sbx_rename_rec,to); SAME(k_rename,sbx_rename_rec,to_bytes);
SIZE(k_dirent_meta,sbx_dirent_meta); SAME(k_dirent_meta,sbx_dirent_meta,length); SAME(k_dirent_meta,sbx_dirent_meta,type); SAME(k_dirent_meta,sbx_dirent_meta,name_bytes); SAME(k_dirent_meta,sbx_dirent_meta,size); SAME(k_dirent_meta,sbx_dirent_meta,modified); SAME(k_dirent_meta,sbx_dirent_meta,created); SAME(k_dirent_meta,sbx_dirent_meta,identity); SAME(k_dirent_meta,sbx_dirent_meta,reserved);
SIZE(k_seek,sbx_seek_rec); SAME(k_seek,sbx_seek_rec,fd); SAME(k_seek,sbx_seek_rec,offset); SAME(k_seek,sbx_seek_rec,whence); SAME(k_seek,sbx_seek_rec,position);
SIZE(k_ioctl,sbx_ioctl_rec); SAME(k_ioctl,sbx_ioctl_rec,fd); SAME(k_ioctl,sbx_ioctl_rec,request); SAME(k_ioctl,sbx_ioctl_rec,buffer); SAME(k_ioctl,sbx_ioctl_rec,bytes);
SIZE(k_exit,sbx_exit_rec); SAME(k_exit,sbx_exit_rec,status);
SIZE(k_fb_mode,sbx_fb_mode); SAME(k_fb_mode,sbx_fb_mode,width); SAME(k_fb_mode,sbx_fb_mode,height); SAME(k_fb_mode,sbx_fb_mode,pitch); SAME(k_fb_mode,sbx_fb_mode,format); SAME(k_fb_mode,sbx_fb_mode,buffer_bytes);
SIZE(k_fb_set,sbx_fb_set_rec); SAME(k_fb_set,sbx_fb_set_rec,format); SIZE(k_fb_palette,sbx_fb_palette_rec); SAME(k_fb_palette,sbx_fb_palette_rec,rgb);
SIZE(k_fb_present,sbx_fb_present_rec); SAME(k_fb_present,sbx_fb_present_rec,x); SAME(k_fb_present,sbx_fb_present_rec,y); SAME(k_fb_present,sbx_fb_present_rec,width); SAME(k_fb_present,sbx_fb_present_rec,height);
SIZE(k_mmap,sbx_mmap_rec); SAME(k_mmap,sbx_mmap_rec,fd); SAME(k_mmap,sbx_mmap_rec,offset); SAME(k_mmap,sbx_mmap_rec,length); SAME(k_mmap,sbx_mmap_rec,protection); SAME(k_mmap,sbx_mmap_rec,flags); SAME(k_mmap,sbx_mmap_rec,address);
SIZE(k_startup,sbx_startup); SAME(k_startup,sbx_startup,size); SAME(k_startup,sbx_startup,major); SAME(k_startup,sbx_startup,minor); SAME(k_startup,sbx_startup,generation); SAME(k_startup,sbx_startup,trial); SAME(k_startup,sbx_startup,image_bytes); SAME(k_startup,sbx_startup,stack_bytes); SAME(k_startup,sbx_startup,handles);
SIZE(k_io,sbx_dirents_rec); SAME(k_io,sbx_dirents_rec,fd); SAME(k_io,sbx_dirents_rec,buffer); _Static_assert(offsetof(struct k_io,count)==offsetof(sbx_dirents_rec,bytes)&&offsetof(struct k_io,transferred)==offsetof(sbx_dirents_rec,transferred),"dirents");
SIZE(k_dirent,sbx_dirent); SAME(k_dirent,sbx_dirent,length); SAME(k_dirent,sbx_dirent,type); SAME(k_dirent,sbx_dirent,name_bytes); SAME(k_dirent,sbx_dirent,size);
SIZE(k_health,sbx_health_rec); SAME(k_health,sbx_health_rec,generation); SAME(k_health,sbx_health_rec,sequence);
SIZE(k_clock,sbx_clock_rec); SAME(k_clock,sbx_clock_rec,clock); SAME(k_clock,sbx_clock_rec,ns);
SIZE(k_sleep,sbx_sleep_rec); SAME(k_sleep,sbx_sleep_rec,deadline);
SIZE(k_eink_info,sbx_eink_info); SAME(k_eink_info,sbx_eink_info,page_bytes); SAME(k_eink_info,sbx_eink_info,max_per_minute); SIZE(k_eink_title,sbx_eink_title_rec); SAME(k_eink_title,sbx_eink_title_rec,flags); SAME(k_eink_title,sbx_eink_title_rec,name_bytes); SAME(k_eink_title,sbx_eink_title_rec,name); SAME(k_eink_title,sbx_eink_title_rec,mark); SAME(k_eink_title,sbx_eink_title_rec,build_bytes); SAME(k_eink_title,sbx_eink_title_rec,build);
SIZE(k_input_event,sbx_input_event); SAME(k_input_event,sbx_input_event,ns); SAME(k_input_event,sbx_input_event,type); SAME(k_input_event,sbx_input_event,code); SAME(k_input_event,sbx_input_event,value);
SIZE(k_input_info,sbx_input_info); SAME(k_input_info,sbx_input_info,flags); SAME(k_input_info,sbx_input_info,instance); SAME(k_input_info,sbx_input_info,queue_records); SAME(k_input_info,sbx_input_info,keys); SAME(k_input_info,sbx_input_info,axes); SAME(k_input_info,sbx_input_info,axis_min); SAME(k_input_info,sbx_input_info,axis_max);
SIZE(k_input_state,sbx_input_state); SAME(k_input_state,sbx_input_state,keys); SAME(k_input_state,sbx_input_state,axes);
_Static_assert(K_ABI_MAJOR==SBX_ABI_MAJOR&&K_ABI_MINOR==SBX_ABI_MINOR,"ABI version");
#ifdef __arm__
#include <sandbox/syscall.h>
/* Every wrapper compiles to ARM-state svc code. */
int32_t use_all(uint64_t fd,char *b) {
 uint64_t n,f; sbx_fb_mode m; int32_t s=sbx_open("/sd/x",5,SBX_O_RDONLY,&f);
 s|=sbx_read(fd,b,4,&n); s|=sbx_log(fd,"hi",2); s|=sbx_fb_query(fd,&m); s|=sbx_fb_present(fd,0,0,1,1); s|=sbx_close(fd); void *a; s|=sbx_fb_map(fd,4096,&a);
 sbx_input_info ii; sbx_input_state is; s|=sbx_getdents(fd,b,64,&n); s|=sbx_health(1,1); s|=sbx_input_query(fd,&ii); s|=sbx_input_state_get(fd,&is);
 uint64_t ns=0; s|=sbx_clock_ns(&ns); s|=sbx_sleep_until(ns+1000000u);
 s|=sbx_seek(fd,0,&n);
 s|=sbx_fsync(fd);s|=sbx_getdents_meta(fd,b,64,&n);s|=sbx_rename("/sd/OS/CACHE/A.TMP",18,"/sd/OS/CACHE/A.IDX",18);
 s|=sbx_getdents_long_meta(fd,b,280,&n);
 sbx_eink_info ei; sbx_eink_title_rec et; s|=sbx_eink_query(fd,&ei); s|=sbx_eink_title(fd,&et,"X",1,(const uint8_t *)b,"V1"); s|=sbx_eink_clear(fd); s|=sbx_eink_page(fd,(const uint8_t *)b);
 sbx_startup_ext startup={{48,1,6,1,0,0,0,0},1};
 s|=sbx_app_arena(&startup.prefix,1,&a,&n);
 if(s==-1) sbx_exit(1);
 return s;
}
#endif
