/*
 *  Main function for 'Face Detection on ARM Target using Code Generation' example
 */


#include "faceDetectionARMKernel_initialize.h"
#include "faceDetectionARMKernel.h"
#include "faceDetectionARMKernel_terminate.h"
#include "helperOpenCVWebcam.hpp"
#include "helperOpenCVVideoViewer.hpp"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdio.h>
#include <linux/videodev2.h>
#include "fbtools.h"
#include "g2d.h"

#define FRAME_WIDTH  640
#define FRAME_HEIGHT 480 

#define TRUE 1
#define FALSE 0

#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define PAGE_MASK (~(PAGE_SIZE-1))
#define CLEAR(x) memset(&(x), 0, sizeof(x))
#define MXCFB_SET_OVERLAY_POS   _IOWR('F', 0x24, struct mxcfb_pos)
#define MXCFB_SET_GBL_ALPHA     _IOW('F', 0x21, struct mxcfb_gbl_alpha)

struct buffer {
        void   *start;
        size_t  length;
};

struct mxcfb_pos {
        __u16 x;
        __u16 y;
};

struct mxcfb_gbl_alpha {
        int enable;
        int alpha;
};

static const char            *dev_name="/dev/video2";
//static enum io_method   io = IO_METHOD_MMAP;
static int              fd = -1;
struct buffer          *buffers;
static unsigned int     n_buffers;
static int              out_buf;
static int              force_format=1;
static int              frame_count = 0x7FFFFFFF;
static FBDEV fbdev;
struct g2d_buf *g2d_buffers[4];

int fb_open(PFBDEV pFbdev)
{
  struct mxcfb_pos pos;
  struct mxcfb_gbl_alpha alpha;
  int fd_fb_bg = 0;

  pFbdev->fb = open(pFbdev->dev, O_RDWR);
  if(pFbdev->fb < 0)
  {
    printf("Error opening %s: %m. Check kernel config\n", pFbdev->dev);
    return FALSE;
  }

  if (-1 == ioctl(pFbdev->fb,FBIOGET_VSCREENINFO,&(pFbdev->fb_var)))
  {
    printf("ioctl FBIOGET_VSCREENINFO\n");
    return FALSE;
  }
  if (-1 == ioctl(pFbdev->fb,FBIOGET_FSCREENINFO,&(pFbdev->fb_fix)))
  {
    printf("ioctl FBIOGET_FSCREENINFO\n");
    return FALSE;
  }

  pos.x = 0;
  pos.y = 0;
  if (ioctl(pFbdev->fb, MXCFB_SET_OVERLAY_POS, &pos) < 0) {
    printf("fb_display_setup MXCFB_SET_OVERLAY_POS failed\n");
    return FALSE;
  }
  //printf("xres: %d, yres: %d, smem_len: %d\n", pFbdev->fb_var.xres, pFbdev->fb_var.yres, pFbdev->fb_fix.smem_len);
  pFbdev->fb_var.xres = 640;
  pFbdev->fb_var.yres = 480;
  pFbdev->fb_var.yres_virtual = pFbdev->fb_var.yres;

  if (ioctl(pFbdev->fb, FBIOPUT_VSCREENINFO, &(pFbdev->fb_var)) < 0) {
      printf("fb_display_setup FBIOPUET_VSCREENINFO failed\n");
      return FALSE;
  }
  if (-1 == ioctl(pFbdev->fb,FBIOGET_FSCREENINFO,&(pFbdev->fb_fix)))
  {
    printf("ioctl FBIOGET_FSCREENINFO\n");
    return FALSE;
  }
  if ((fd_fb_bg = open("/dev/fb0", O_RDWR )) < 0) {
    printf("Unable to open bg frame buffer\n");
    return FALSE;
  }

  alpha.alpha = 0;
  alpha.enable = 1;
  if (ioctl(fd_fb_bg, MXCFB_SET_GBL_ALPHA, &alpha) < 0) {
     printf("Set global alpha failed\n");
     close(fd_fb_bg);
     return FALSE;
  }

  ioctl(pFbdev->fb, FBIOBLANK, FB_BLANK_UNBLANK);
  printf("xres: %d, yres: %d, smem_len: %d\n", pFbdev->fb_var.xres, pFbdev->fb_var.yres, pFbdev->fb_fix.smem_len);
  //map physics address to virtual address
  pFbdev->fb_mem_offset = (unsigned long)(pFbdev->fb_fix.smem_start) & (~PAGE_MASK);
  pFbdev->fb_mem = (unsigned long int)mmap(NULL, pFbdev->fb_fix.smem_len + pFbdev->fb_mem_offset, PROT_READ | PROT_WRITE, MAP_SHARED, pFbdev->fb, 0);
  if (-1L == (long) pFbdev->fb_mem)
  {
    printf("mmap error! mem:%d offset:%d\n", pFbdev->fb_mem,
    pFbdev->fb_mem_offset);
    return FALSE;
  }

  return TRUE;
}

//close frame buffer
int fb_close(PFBDEV pFbdev)
{
  munmap((void *)pFbdev->fb_mem, pFbdev->fb_fix.smem_len+pFbdev->fb_mem_offset);
  close(pFbdev->fb);
  pFbdev->fb=-1;
}

static void errno_exit(const char *s)
{
        fprintf(stderr, "%s error %d, %s\\n", s, errno, strerror(errno));
        exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg)
{
        int r;

        do {
                r = ioctl(fh, request, arg);
        } while (-1 == r && EINTR == errno);

        return r;
}

void *g2dHandle;
struct g2d_surface src, dst;
g2d_buf *g2d_inRGB;
void g2d_colorspace_convert(struct g2d_buf *srcBuf, int w, int h, uint8_T *rgbBuf) {
   int count=0;
   
   src.planes[0] = srcBuf->buf_paddr;
   src.left = 0;
   src.top = 0;
   src.right = w;
   src.bottom = h;
   src.stride = w;
   src.width = w;
   src.height = h;
   src.rot =G2D_ROTATION_0;
   src.format = G2D_YUYV;

   dst.planes[0] = g2d_inRGB->buf_paddr;
   //dst.planes[0] = dstBuf->buf_paddr;
   dst.left = 0;
   dst.top = 0;
   dst.right = w;
   dst.bottom = h;
   dst.stride = w;
   dst.width = w;
   dst.height = h;
   dst.rot =G2D_ROTATION_0;
   dst.format = G2D_BGRX8888;
   //dst.format = G2D_RGB565;


   g2d_blit(g2dHandle, &src, &dst);
   g2d_finish(g2dHandle);

   for(int i=0; i<w*h*4; i++){
	if(i%4!=3){
		rgbBuf[count++] = ((uint8_T *)(g2d_inRGB->buf_vaddr))[i];
	}	
   }
   //memcpy((char *)fbdev.fb_mem, rgbBuf, w*h*3);
}

static int read_frame(uint8_T *rgbBuf)
{
        struct v4l2_buffer buf;
        unsigned int i;
	CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) {
                switch (errno) {
                case EAGAIN:
                        return 0;

                case EIO:
                        /* Could ignore EIO, see spec. */

                        /* fall through */

                default:
                        errno_exit("VIDIOC_DQBUF");
                }
        }

        assert(buf.index < n_buffers);
        memcpy(g2d_buffers[buf.index]->buf_vaddr, buffers[buf.index].start, buf.bytesused);
        //g2d_colorspace_convert(g2d_buffers[buf.index], 640, 480, fbdev.fb_fix.smem_start);
        g2d_colorspace_convert(g2d_buffers[buf.index], 640, 480, rgbBuf);
        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                errno_exit("VIDIOC_QBUF");
	
	return 1;
}

static void stop_capturing(void)
{
        enum v4l2_buf_type type;

        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
                errno_exit("VIDIOC_STREAMOFF");
}

static void start_capturing(void)
{
        unsigned int i;
        enum v4l2_buf_type type;
	for (i = 0; i < n_buffers; ++i) {
                struct v4l2_buffer buf;

                CLEAR(buf);
                buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory = V4L2_MEMORY_MMAP;
                buf.index = i;

                if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                        errno_exit("VIDIOC_QBUF");
        }
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
                errno_exit("VIDIOC_STREAMON");
}

static void uninit_device(void)
{
        unsigned int i;

        for (i = 0; i < n_buffers; ++i)
                if (-1 == munmap(buffers[i].start, buffers[i].length))
                        errno_exit("munmap");
        free(buffers);
}

static void init_mmap(void)
{
        struct v4l2_requestbuffers req;

        CLEAR(req);

        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) {
                if (EINVAL == errno) {
                        fprintf(stderr, "%s does not support "
                                 "memory mappingn", dev_name);
                        exit(EXIT_FAILURE);
                } else {
                        errno_exit("VIDIOC_REQBUFS");
                }
        }

        if (req.count < 2) {
                fprintf(stderr, "Insufficient buffer memory on %s\\n",
                         dev_name);
                exit(EXIT_FAILURE);
        }

        buffers = (buffer *)calloc(req.count, sizeof(*buffers));

        if (!buffers) {
                fprintf(stderr, "Out of memory\\n");
                exit(EXIT_FAILURE);
        }

        for (n_buffers = 0; n_buffers < req.count; ++n_buffers) {
                struct v4l2_buffer buf;

                CLEAR(buf);

                buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buf.memory      = V4L2_MEMORY_MMAP;
                buf.index       = n_buffers;

                if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
                        errno_exit("VIDIOC_QUERYBUF");

                buffers[n_buffers].length = buf.length;
		buffers[n_buffers].start =
                        mmap(NULL /* start anywhere */,
                              buf.length,
                              PROT_READ | PROT_WRITE /* required */,
                              MAP_SHARED /* recommended */,
                              fd, buf.m.offset);

                if (MAP_FAILED == buffers[n_buffers].start)
                        errno_exit("mmap");
                g2d_buffers[n_buffers] = g2d_alloc(640*480*2, 0);//alloc physical contiguous memory for source image data
                if(!g2d_buffers[n_buffers]) {
                        printf("Fail to allocate physical memory for image buffer!\n");
                        return;
                }

        }
}

static void init_device(void)
{
        struct v4l2_capability cap;
        struct v4l2_cropcap cropcap;
        struct v4l2_crop crop;
        struct v4l2_format fmt;
        unsigned int min;

        if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
                if (EINVAL == errno) {
                        fprintf(stderr, "%s is no V4L2 device\\n",
                                 dev_name);
                        exit(EXIT_FAILURE);
                } else {
                        errno_exit("VIDIOC_QUERYCAP");
                }
        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
                fprintf(stderr, "%s is no video capture device\\n",
                         dev_name);
                exit(EXIT_FAILURE);
        }

        if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
                fprintf(stderr, "%s does not support streaming i/o\\n",
                         dev_name);
                exit(EXIT_FAILURE);
        }

	/* Select video input, video standard and tune here. */


        CLEAR(cropcap);

        cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap)) {
                crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                crop.c = cropcap.defrect; /* reset to default */

                if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop)) {
                        switch (errno) {
                        case EINVAL:
                                /* Cropping not supported. */
                                break;
                        default:
                                /* Errors ignored. */
                                break;
                        }
                }
        } else {
                /* Errors ignored. */
        }


        CLEAR(fmt);

        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (force_format) {
                fmt.fmt.pix.width       = 640;
                fmt.fmt.pix.height      = 480;
                fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
                //fmt.fmt.pix.field       = V4L2_FIELD_INTERLACED;

                if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
                        errno_exit("VIDIOC_S_FMT");

                /* Note VIDIOC_S_FMT may change width and height. */
        } else {
                /* Preserve original settings as set by v4l2-ctl for example */
                if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
                        errno_exit("VIDIOC_G_FMT");
        }
	/* Buggy driver paranoia. */
        min = fmt.fmt.pix.width * 2;
        if (fmt.fmt.pix.bytesperline < min)
                fmt.fmt.pix.bytesperline = min;
        min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
        if (fmt.fmt.pix.sizeimage < min)
                fmt.fmt.pix.sizeimage = min;

        init_mmap();
}

static void close_device(void)
{
        if (-1 == close(fd))
                errno_exit("close");

        fd = -1;
}
static void open_device(void)
{
        struct stat st;

        if (-1 == stat(dev_name, &st)) {
                fprintf(stderr, "Cannot identify '%s': %d, %s\\n",
                         dev_name, errno, strerror(errno));
                exit(EXIT_FAILURE);
        }

        if (!S_ISCHR(st.st_mode)) {
                fprintf(stderr, "%s is no devicen", dev_name);
                exit(EXIT_FAILURE);
        }

        fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);

        if (-1 == fd) {
                fprintf(stderr, "Cannot open '%s': %d, %s\\n",
                         dev_name, errno, strerror(errno));
                exit(EXIT_FAILURE);
        }
}

int main()
{
    /* Allocate input and output image buffers */
    uint8_T inRGB[FRAME_WIDTH * FRAME_HEIGHT * 3],
            outRGB[FRAME_WIDTH * FRAME_HEIGHT * 3];
    clock_t begin , end;

    /* Local variables */
    const char *winNameOut = "Output Video";
    const int frameWidth = FRAME_WIDTH, frameHeight = FRAME_HEIGHT;
    unsigned uRunTime_capture, uRunTime_face, uRunTime_draw;
    void* capture = 0;

    /* Initialize camera */
    capture = (void *)opencvInitCam(frameWidth, frameHeight);

    /* Initialize video viewer */
    opencvInitVideoViewer(winNameOut);

    /*memset(&fbdev, 0, sizeof(FBDEV));
    strcpy(fbdev.dev, "/dev/fb1");
    if(fb_open(&fbdev)==FALSE)
    {
       printf("open frame buffer error\n");
       return 0;
    }
    
    g2d_inRGB = g2d_alloc(640*480*4, 0);//alloc physical contiguous memory for source image data
    if(!g2d_inRGB) {
           printf("Fail to allocate physical memory for image buffer!\n");
           return 0;
    }
    if (g2d_open(&g2dHandle) == -1 || g2dHandle == NULL) {
            printf("Fail to open g2d device!\n");
            return 0;
    }
    open_device();
    init_device();
    start_capturing();*/
    /* Call MATLAB Coder generated initialize function */
    faceDetectionARMKernel_initialize();

    /* Exit while loop on Escape.
     *   - Make sure you press escape key while video viewer windows is on focus.
     *   - Program waits for only 10 ms for a pressed key. You may  need to 
     *     press Escape key multiple times before it gets  detected.
     */    
    while (1)
    {
        begin = clock();      
        /* Capture frame from camera */
        opencvCaptureRGBFrameAndCopy(capture, inRGB);
	//read_frame(inRGB);
	/*for (;;) {
                fd_set fds;
                struct timeval tv;
                int r;

                FD_ZERO(&fds);
                FD_SET(fd, &fds);

                // Timeout.
                tv.tv_sec = 2;
                tv.tv_usec = 0;

                r = select(fd + 1, &fds, NULL, NULL, &tv);

                if (-1 == r) {
                        if (EINTR == errno)
                                continue;
                        errno_exit("select");
                }

                if (0 == r) {
                        fprintf(stderr, "select timeout\\n");
                        exit(EXIT_FAILURE);
                }

                if (read_frame(g2d_inRGB))
                        break;
                //EAGAIN - continue select loop.
        }*/
	end = clock();
	uRunTime_capture = (end - begin) * 1.0 / CLOCKS_PER_SEC * 1000;
        /* **********************************************************
         * Call MATLAB Coder generated kernel function.             *
         * This function detects faces and inserts bounding boxes   *
         * around faces.                                            *
         * MATLAB API: outRGB = faceDetectionARMKernel(inRGB) *
         * MATLAB Coder generated C API:                            *
         *        void faceDetectionARMKernel(                *
         *                      const unsigned char inRGB[921600],  *
         *                            unsigned char outRGB[921600]) *
         * **********************************************************/
        begin = clock();
        //faceDetectionARMKernel(inRGB, (unsigned char*)fbdev.fb_mem);
        //faceDetectionARMKernel((const unsigned char*)g2d_inRGB->buf_vaddr, (unsigned char*)fbdev.fb_mem);
        faceDetectionARMKernel(inRGB, outRGB);
	end = clock();
	uRunTime_face = (end - begin) * 1.0 / CLOCKS_PER_SEC * 1000;

        /* Display output image */
        begin = clock();
        opencvDisplayRGB(outRGB, frameHeight, frameWidth, winNameOut);
	end = clock();
	uRunTime_draw = (end - begin) * 1.0 / CLOCKS_PER_SEC * 1000;
	printf("kevin uRuntime_capture is %d ms, uRuntime_face is %d ms, uRuntime_draw is %d ms\n", uRunTime_capture, uRunTime_face, uRunTime_draw);
    }

    /* Call MATLAB Coder generated terminate function */
    faceDetectionARMKernel_terminate();
    /* 0 - success */
    return 0;
}
