#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <linux/mxcfb.h>
#include <linux/ipu.h>

struct command_line {
    char* src_dev;
    char* dst_dev;
    int fps;
};

struct display_data {
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    int fd;
};

static struct command_line cmd = { "/dev/fb0", "/dev/fb1", 30};
static struct display_data src, dst;
static struct ipu_task task;

static char *options = "i:o:f:";
char *usage = "Usage: ./fbcp <options>\n "\
            "  -i <src framebuffer> copy from framebuffer \n "\
            "   default is /dev/fb0 \n "\
            "  -o <dst framebuffer> copy to framebuffer \n "\
            "   default is /dev/fb1 \n "\
            "  -f <frame rate> display framerate \n "\
            "   default is 30 \n ";

int parse_args(int argc, char *argv[])
{
    int status = 0, opt;

    do {
        opt = getopt(argc, argv, options);
        switch (opt)
        {
        case 'i':
            cmd.src_dev = optarg;
            break;
        case 'o':
            cmd.dst_dev = optarg;
            break;
        case 'f':
            cmd.fps = atoi(optarg);
            break;
        case -1:
            break;
        default:
            status = -1;
            break;
        }
    } while ((opt != -1) && (status == 0));

    return status;
}

static unsigned int fmt_to_bpp(unsigned int pixelformat)
{
    unsigned int bpp;
 
        switch (pixelformat)
        {
                case IPU_PIX_FMT_RGB565:
               /*interleaved 422*/
                case IPU_PIX_FMT_YUYV:
                case IPU_PIX_FMT_UYVY:
                /*non-interleaved 422*/
                case IPU_PIX_FMT_YUV422P:
                case IPU_PIX_FMT_YVU422P:
                        bpp = 16;
                        break;
                case IPU_PIX_FMT_BGR24:
                case IPU_PIX_FMT_RGB24:
                case IPU_PIX_FMT_YUV444:
                case IPU_PIX_FMT_YUV444P:
                        bpp = 24;
                        break;
                case IPU_PIX_FMT_BGR32:
                case IPU_PIX_FMT_BGRA32:
                case IPU_PIX_FMT_RGB32:
                case IPU_PIX_FMT_RGBA32:
                case IPU_PIX_FMT_ABGR32:
                        bpp = 32;
                        break;
                /*non-interleaved 420*/
                case IPU_PIX_FMT_YUV420P:
                case IPU_PIX_FMT_YVU420P:
                case IPU_PIX_FMT_YUV420P2:
                case IPU_PIX_FMT_NV12:
    case IPU_PIX_FMT_TILED_NV12:
                        bpp = 12;
                        break;
                default:
                        bpp = 8;
                        break;
        }
        return bpp;
}

void control_fps(struct timespec *begin_clock_time, struct timespec *end_clock_time, float fps)
{
    float fps_interval = 1000.0/fps;
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 200;

    clock_gettime(CLOCK_MONOTONIC, end_clock_time);

    while ((end_clock_time->tv_sec * 1000.0 + end_clock_time->tv_nsec / 1000.0 / 1000.0) -
            (begin_clock_time->tv_sec * 1000.0 + begin_clock_time->tv_nsec / 1000.0 / 1000.0)
             < fps_interval) {
        select(0, NULL, NULL, NULL, &tv);
        clock_gettime(CLOCK_MONOTONIC, end_clock_time);
    }
    memcpy(begin_clock_time, end_clock_time, sizeof(struct timespec));
}

static int init_display_device(struct display_data *data, char *dev)
{
    if((data->fd = open(dev, O_RDWR, 0)) < 0) {
        printf("Unable to open %s\n", dev);
        return -1;
    }

    if(ioctl(data->fd, FBIOGET_VSCREENINFO, &data->var) < 0) {
        printf("Unable to get %s var screeninfo\n", dev);
        return -1;
    }

    if(ioctl(data->fd, FBIOGET_FSCREENINFO, &data->fix) < 0) {
        printf("Unable to get %s fix screeninfo\n", dev);
        return -1;
    }
}

int main (int argc, char *argv[])
{
    int ret = 0;
    int fd_ipu = 0;
    int display_x = 0;
    int display_y = 0;
    float src_radio = 0;
    float dst_radio = 0;
    int display_offset = 0;
    static struct timespec begin_clock_time, end_clock_time;

    ret = parse_args(argc, argv);
    if (ret) {
        goto usage;
    }

    // Open IPU device
    fd_ipu = open("/dev/mxc_ipu", O_RDWR, 0);
    if (fd_ipu < 0) {
        printf("open ipu dev fail\n");
        return -1;
    }

    if(init_display_device(&src, cmd.src_dev) < 0) {
        printf("init %s device error!\n", cmd.src_dev);
        return -1;
    }

    if(init_display_device(&dst, cmd.dst_dev) < 0) {
        printf("init %s device error!\n", cmd.dst_dev);
        return -1;
    }

    // Input image size and format
    task.input.width    = src.var.xres;
    task.input.height   = src.var.yres;
    task.input.crop.w = src.var.xres;
    task.input.crop.h = src.var.yres - 1;
    task.input.crop.pos.x = 0;
    task.input.crop.pos.y = 1;
    task.input.format   = v4l2_fourcc('B', 'G', 'R', 'A');
    task.input.paddr = src.fix.smem_start;

    src_radio = (float)src.var.xres / (float)src.var.yres;
    dst_radio = (float)dst.var.xres / (float)dst.var.yres;
    if(src_radio != dst_radio) {
        if(dst.var.xres >= dst.var.yres) {
            task.output.crop.h = dst.var.yres;
            task.output.crop.w = dst.var.yres * src_radio;
        }
        else {
            task.output.crop.w = dst.var.xres;
            task.output.crop.h = dst.var.xres * src_radio;
        }
    }
    else {
        task.output.crop.w = dst.var.xres;
        task.output.crop.h = dst.var.yres;
    }
    display_x = (dst.var.xres - task.output.crop.w) / 2;
    display_y = (dst.var.yres - task.output.crop.h) / 2;

    // Output image size and format
    task.output.width = dst.var.xres;
    task.output.height = dst.var.yres;
    task.output.format  = v4l2_fourcc('B', 'G', 'R', 'A');
    display_offset = (dst.var.xres * display_y + display_x) * fmt_to_bpp(task.output.format)/8;
    task.output.paddr = dst.fix.smem_start + display_offset;

    while (1) {
        ret = ioctl(fd_ipu, IPU_QUEUE_TASK, &task);
        if (ret < 0) {
            printf("ioct IPU_QUEUE_TASK fail\n");
            return ret;
        }
        control_fps(&begin_clock_time, &end_clock_time, cmd.fps);
    }

    if (fd_ipu)
        close(fd_ipu);
    if (src.fd)
        close(src.fd);
    if (dst.fd)
        close(dst.fd);

    return 0;

usage:
    printf("%s", usage);
    return -1;
}
