#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <string.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <g2d.h>

struct display_data {
    struct g2d_surface surface;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    int fd;
};

struct command_line {
    char* src_dev;
    char* dst_dev;
    int fps;
};

static struct display_data src, dst;
static struct command_line cmd = { "/dev/fb0", "/dev/fb1", 30};

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

static int init_display_data(struct display_data *data, char *dev)
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
    int err = 0;
    void *handle;
    int display_x = 0;
    int display_y = 0;
    int display_width = 0;
    int display_height = 0;
    float src_radio = 0;
    float dst_radio = 0;
    int display_offset = 0;
    static struct timespec begin_clock_time;
    static struct timespec end_clock_time;

    err = parse_args(argc, argv);
    if (err) {
        goto usage;
    }

    if(init_display_data(&src, cmd.src_dev) < 0) {
        printf("init %s data error!\n", cmd.src_dev);
        return -1;
    }

    if(init_display_data(&dst, cmd.dst_dev) < 0) {
        printf("init %s data error!\n", cmd.dst_dev);
        return -1;
    }

    // Input Info
    src.surface.left = 0;
    src.surface.top = 0;
    src.surface.right = src.var.xres;
    src.surface.bottom = src.var.yres;
    src.surface.width = src.var.xres;
    src.surface.height = src.var.yres;
    src.surface.stride = src.var.xres;
    src.surface.rot = G2D_ROTATION_0;
    src.surface.format = G2D_BGRA8888;
    src.surface.planes[0] = src.fix.smem_start;

    // Output Info
    src_radio = (float)src.var.xres / (float)src.var.yres;
    dst_radio = (float)dst.var.xres / (float)dst.var.yres;
    if(src_radio != dst_radio) {
        if(dst.var.xres >= dst.var.yres) {
            display_height = dst.var.yres;
            display_width = dst.var.yres * src_radio;
        }
        else {
            display_width = dst.var.xres;
            display_height = dst.var.xres * src_radio;
        }
    }
    else {
        display_width = dst.var.xres;
        display_height = dst.var.yres;
    }
    dst.surface.left = (dst.var.xres - display_width) / 2;
    dst.surface.top = (dst.var.yres - display_height) / 2;

    dst.surface.right = dst.var.xres - (dst.var.xres - dst.surface.left - display_width);
    dst.surface.bottom = dst.var.yres - (dst.var.yres - dst.surface.top - display_height);
    dst.surface.width = dst.var.xres;
    dst.surface.height = dst.var.yres;
    dst.surface.stride = dst.var.xres;
    dst.surface.rot = G2D_ROTATION_0;
    dst.surface.format = G2D_BGRA8888;
    dst.surface.planes[0] = dst.fix.smem_start;

    if(g2d_open(&handle)) {
        printf("g2d open fail\n");
        return -1;
    }

    while(1) {
        if(g2d_blit(handle, &src.surface, &dst.surface)) {
            printf("g2d blit fail\n");
            return -1;
        }

        if(g2d_finish(handle)) {
            printf("g2d finish fail\n");
            return -1;
        }
        control_fps(&begin_clock_time, &end_clock_time, cmd.fps);
    }

    g2d_close(handle);

    if (src.fd)
        close(src.fd);
    if (dst.fd)
        close(dst.fd);

    return 0;

usage:
    printf("%s", usage);
    return -1;
}
