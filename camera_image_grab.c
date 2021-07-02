#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <getopt.h> /* getopt_long() */

#include <fcntl.h> /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

#include <linux/videodev2.h>
#include <linux/v4l2-controls.h>
#include "bmp.h"

#define CLEAR(x) memset(&(x), 0, sizeof(x))

struct buffer
{
    void *start;
    size_t length;
};

enum io_method
{
    IO_METHOD_READ,
    IO_METHOD_MMAP,
    IO_METHOD_USERPTR,
};

struct v4l2_format fmt;
struct v4l2_buffer buf;
struct v4l2_requestbuffers req;
enum v4l2_buf_type type;
fd_set fds;
struct timeval tv;
int r, fd = -1;
unsigned int i, n_buffers;
char out_name[256];
FILE *fout;
struct buffer *buffers;
int frame_width = 640;
int frame_height = 480;

struct v4l2_queryctrl queryctrl;
struct v4l2_querymenu querymenu;
struct v4l2_control control;
struct v4l2_query_ext_ctrl query_ext_ctrl;

char *dev_name = "/dev/video0";
int force_format = 1;
static enum io_method io = IO_METHOD_MMAP;

static void errno_exit(const char *s)
{
    fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
    exit(EXIT_FAILURE);
}

static int xioctl(int fh, int request, void *arg)
{
    int r;

    do
    {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}

static void init_read(unsigned int buffer_size)
{
    buffers = calloc(1, sizeof(*buffers));

    if (!buffers)
    {
        fprintf(stderr, "Out of memory\\n");
        exit(EXIT_FAILURE);
    }

    buffers[0].length = buffer_size;
    buffers[0].start = malloc(buffer_size);

    if (!buffers[0].start)
    {
        fprintf(stderr, "Out of memory\\n");
        exit(EXIT_FAILURE);
    }
}

static void init_mmap(void)
{
    struct v4l2_requestbuffers req;

    CLEAR(req);

    req.count = 4; // what is 4? the buffer size of frame to dynamically saved? do it related to n_buffers
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP; // set the video mode as memory address mapping

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req)) //
    {
        if (EINVAL == errno)
        {
            fprintf(stderr, "%s does not support "
                            "memory mapping",
                    dev_name);
            exit(EXIT_FAILURE);
        }
        else
        {
            errno_exit("VIDIOC_REQBUFS");
        }
    }

    if (req.count < 2) // request failure
    {
        fprintf(stderr, "Insufficient buffer memory on %s\\n",
                dev_name);
        exit(EXIT_FAILURE);
    }

    buffers = calloc(req.count, sizeof(*buffers)); // create space in buffers point, ready to read

    if (!buffers)
    {
        fprintf(stderr, "Out of memory\\n");
        exit(EXIT_FAILURE);
    }

    for (n_buffers = 0; n_buffers < req.count; ++n_buffers) // for each buffer container in buffers point, store image!
    {
        struct v4l2_buffer buf;

        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = n_buffers;

        if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf)) // read the info. of iamge buffer, like buffer length of data
            errno_exit("VIDIOC_QUERYBUF");

        buffers[n_buffers].length = buf.length;
        buffers[n_buffers].start =          // this is a memory mapping function and return address of data, avoiding hard copy
            mmap(NULL /* start anywhere */, // usually set to NULL
                 buf.length,
                 PROT_READ | PROT_WRITE /* required */,
                 MAP_SHARED /* recommended */,
                 fd, buf.m.offset);

        if (MAP_FAILED == buffers[n_buffers].start)
            errno_exit("mmap");
    }
}

static void init_userp(unsigned int buffer_size)
{
    struct v4l2_requestbuffers req;

    CLEAR(req);

    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_USERPTR;

    if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req))
    {
        if (EINVAL == errno)
        {
            fprintf(stderr, "%s does not support "
                            "user pointer i/on",
                    dev_name);
            exit(EXIT_FAILURE);
        }
        else
        {
            errno_exit("VIDIOC_REQBUFS");
        }
    }

    buffers = calloc(4, sizeof(*buffers));

    if (!buffers)
    {
        fprintf(stderr, "Out of memory\\n");
        exit(EXIT_FAILURE);
    }

    for (n_buffers = 0; n_buffers < 4; ++n_buffers)
    {
        buffers[n_buffers].length = buffer_size;
        buffers[n_buffers].start = malloc(buffer_size);

        if (!buffers[n_buffers].start)
        {
            fprintf(stderr, "Out of memory\\n");
            exit(EXIT_FAILURE);
        }
    }
}

static void openDevice(void)
{
    printf("\n   /////////////////\n    OPEN DEVICE\n");

    struct stat st;

    if (-1 == stat(dev_name, &st))
    {
        fprintf(stderr, "Cannot identify '%s': %d, %s\n",
                dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (!S_ISCHR(st.st_mode))
    {
        fprintf(stderr, "%s is no devicen", dev_name);
        exit(EXIT_FAILURE);
    }

    fd = open(dev_name, O_RDWR /* required */ | O_NONBLOCK, 0);

    if (-1 == fd)
    {
        fprintf(stderr, "Cannot open '%s': %d, %s\n",
                dev_name, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
    if (fd < 0)
    {
        perror("Cannot open device");
        exit(EXIT_FAILURE);
    }
}

static int checkDeivce(void)
{
    printf("\n   /////////////////\n    CHECK DEVICE\n");
    struct v4l2_input input;
    int index;

    if (-1 == ioctl(fd, VIDIOC_G_INPUT, &index))
    {
        perror("VIDIOC_G_INPUT");
        exit(EXIT_FAILURE);
    }

    memset(&input, 0, sizeof(input));
    input.index = index;

    if (-1 == ioctl(fd, VIDIOC_ENUMINPUT, &input))
    {
        perror("VIDIOC_ENUMINPUT");
        exit(EXIT_FAILURE);
    }

    printf("Current input: %s\n", input.name); //Current input: Camera 0
    return input.index;
}

static void selectDevice(int index)
{
    printf("\n   /////////////////\n    SELECT DEVICE\n");

    if (-1 == ioctl(fd, VIDIOC_S_INPUT, &index))
    {
        perror("VIDIOC_S_INPUT");
        exit(EXIT_FAILURE);
    }
}

static void closeDevice(void) // close fd
{
    printf("\n   /////////////////\n    CLOSE DEVICE\n");

    if (-1 == close(fd))
        errno_exit("close");

    fd = -1;
}

static void initDevice(void)
{
    printf("\n   /////////////////\n    INIT DEVICE\n");

    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;
    struct v4l2_format fmt;
    unsigned int min;

    if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap)) // check camera's property, read it
    {
        if (EINVAL == errno)
        {
            fprintf(stderr, "%s is no V4L2 device\\n",
                    dev_name);
            exit(EXIT_FAILURE);
        }
        else
        {
            errno_exit("VIDIOC_QUERYCAP");
        }
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) // check device is video device or not
    {
        fprintf(stderr, "%s is no video capture device\\n",
                dev_name);
        exit(EXIT_FAILURE);
    }

    switch (io) // different mode have different operation
    {
    case IO_METHOD_READ:
        if (!(cap.capabilities & V4L2_CAP_READWRITE)) // IO_Method read, check if read&write io is ok
        {
            fprintf(stderr, "%s does not support read i/o\\n",
                    dev_name);
            exit(EXIT_FAILURE);
        }
        break;

    case IO_METHOD_MMAP:
    case IO_METHOD_USERPTR:
        if (!(cap.capabilities & V4L2_CAP_STREAMING))
        {
            fprintf(stderr, "%s does not support streaming i/o\\n",
                    dev_name);
            exit(EXIT_FAILURE);
        }
        break;
    }

    /* Select video input, video standard and tune here. */

    CLEAR(cropcap);

    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // set the type of buffer for transforming video

    if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap))
    {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect; /* reset to default */ // means setting the size crop of image?

        if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop))
        {
            switch (errno)
            {
            case EINVAL:
                /* Cropping not supported. */
                break;
            default:
                /* Errors ignored. */
                break;
            }
        }
    }
    else
    {
        /* Errors ignored. */
    }

    CLEAR(fmt);

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // format of image
    if (force_format)                       // default to 0
    {
        fmt.fmt.pix.width = frame_width;
        fmt.fmt.pix.height = frame_height;

        // fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV; // yuyv format
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24; // yuyv format
        fmt.fmt.pix.field = V4L2_FIELD_INTERLACED;

        if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
            errno_exit("VIDIOC_S_FMT");

        /* Note VIDIOC_S_FMT may change width and height. */
    }

    /* Preserve original settings as set by v4l2-ctl for example */
    if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
        errno_exit("VIDIOC_G_FMT");
    frame_width = fmt.fmt.pix.width;
    frame_height = fmt.fmt.pix.height;

    char fmt_str[8];
    memset(fmt_str,0,8);
    memcpy(fmt_str, &fmt.fmt.pix.pixelformat, 4);
    printf("image format is : %d x %d %s \n ",frame_height, frame_width, fmt_str);

    /* Buggy driver paranoia. */ // wait to understand
    min = fmt.fmt.pix.width * 2;
    if (fmt.fmt.pix.bytesperline < min)
        fmt.fmt.pix.bytesperline = min;
    min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
    if (fmt.fmt.pix.sizeimage < min)
        fmt.fmt.pix.sizeimage = min;

    switch (io) // create the buffer size to store video data
    {
    case IO_METHOD_READ:
        init_read(fmt.fmt.pix.sizeimage);
        break;

    case IO_METHOD_MMAP:
        init_mmap();
        break;

    case IO_METHOD_USERPTR:
        init_userp(fmt.fmt.pix.sizeimage);
        break;
    }
}

static void uninitDevice(void) // release data buffer space
{
    printf("\n   /////////////////\n    UNINIT DEVICE\n");

    unsigned int i;

    switch (io)
    {
    case IO_METHOD_READ:
        free(buffers[0].start);
        break;

    case IO_METHOD_MMAP:
        for (i = 0; i < n_buffers; ++i)
            if (-1 == munmap(buffers[i].start, buffers[i].length))
                errno_exit("munmap");
        break;

    case IO_METHOD_USERPTR:
        for (i = 0; i < n_buffers; ++i)
            free(buffers[i].start);
        break;
    }

    free(buffers);
}

static void startCapturing(void) // make it start stream iamge data
{
    printf("\n   /////////////////\n    START CAPTURING\n");

    unsigned int i;
    enum v4l2_buf_type type;

    switch (io)
    {
    case IO_METHOD_READ:
        /* Nothing to do. */
        break;

    case IO_METHOD_MMAP:                // default mode
        for (i = 0; i < n_buffers; ++i) // for each buffer container
        {
            struct v4l2_buffer buf;

            CLEAR(buf);
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (-1 == xioctl(fd, VIDIOC_QBUF, &buf)) // VIDIOC_QBUF means create a buffer for driver to cache video data, with index i
                errno_exit("VIDIOC_QBUF");
        }
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (-1 == xioctl(fd, VIDIOC_STREAMON, &type)) // start to stream video?(start cache the image data into buffer)
            errno_exit("VIDIOC_STREAMON");
        break;

    case IO_METHOD_USERPTR:
        for (i = 0; i < n_buffers; ++i)
        {
            struct v4l2_buffer buf;

            CLEAR(buf);
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_USERPTR;
            buf.index = i;
            buf.m.userptr = (unsigned long)buffers[i].start;
            buf.length = buffers[i].length;

            if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
                errno_exit("VIDIOC_QBUF");
        }
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
            errno_exit("VIDIOC_STREAMON");
        break;
    }
}

static void stopCapturing(void) // stop the driver from streaming data
{
    printf("\n   /////////////////\n    STOP CAPTURING\n");

    enum v4l2_buf_type type;

    switch (io)
    {
    case IO_METHOD_READ:
        /* Nothing to do. */
        break;

    case IO_METHOD_MMAP:
    case IO_METHOD_USERPTR:
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
            errno_exit("VIDIOC_STREAMOFF");
        break;
    }
}

//------------save image picture captured--------///
int GenBmpFile(const unsigned char *pData, unsigned char bitCountPerPix, unsigned int width, unsigned int height, const char *filename)
{
    FILE *fp = fopen(filename, "wb");
    if(!fp)
    {
        printf("fopen failed : %s, %d\n", __FILE__, __LINE__);
        return 0;
    }
    unsigned int bmppitch = ((width*bitCountPerPix + 31) >> 5) << 2;
    unsigned int filesize = bmppitch*height;
    // unsigned int filesize = width*height*3;
 
    BITMAPFILE bmpfile;
    bmpfile.bfHeader.bfType = 0x4D42;
    bmpfile.bfHeader.bfSize = filesize + sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bmpfile.bfHeader.bfReserved1 = 0;
    bmpfile.bfHeader.bfReserved2 = 0;
    bmpfile.bfHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
 
    bmpfile.biInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmpfile.biInfo.bmiHeader.biWidth = width;
    bmpfile.biInfo.bmiHeader.biHeight = height;
    bmpfile.biInfo.bmiHeader.biPlanes = 3;
    bmpfile.biInfo.bmiHeader.biBitCount = bitCountPerPix;
    bmpfile.biInfo.bmiHeader.biCompression = 0;
    bmpfile.biInfo.bmiHeader.biSizeImage = filesize;
    bmpfile.biInfo.bmiHeader.biXPelsPerMeter = 0;
    bmpfile.biInfo.bmiHeader.biYPelsPerMeter = 0;
    bmpfile.biInfo.bmiHeader.biClrUsed = 0;
    bmpfile.biInfo.bmiHeader.biClrImportant = 0;
 
    fwrite(&(bmpfile.bfHeader), sizeof(BITMAPFILEHEADER), 1, fp);
    fwrite(&(bmpfile.biInfo.bmiHeader), sizeof(BITMAPINFOHEADER), 1, fp);
    fwrite(pData,filesize,1, fp);
//    unsigned char *pEachLinBuf = (unsigned char*)malloc(bmppitch);
//    memset(pEachLinBuf, 0, bmppitch);
//    unsigned char BytePerPix = bitCountPerPix >> 3;
//    unsigned int pitch = width * BytePerPix;
//    if(pEachLinBuf)
//    {
//        int h,w;
//        for(h = height-1; h >= 0; h--)
//        {
//            for(w = 0; w < width; w++)
//            {
//                //copy by a pixel
//                pEachLinBuf[w*BytePerPix+0] = pData[h*pitch + w*BytePerPix + 0];
//                pEachLinBuf[w*BytePerPix+1] = pData[h*pitch + w*BytePerPix + 1];
//                pEachLinBuf[w*BytePerPix+2] = pData[h*pitch + w*BytePerPix + 2];
//            }
//            fwrite(pEachLinBuf, bmppitch, 1, fp);
//
//        }
//        free(pEachLinBuf);
//    }
 
    fclose(fp);
 
    return 1;
}

int GenJpgFile(struct buffer *buffers,const char *filename){
    FILE *fp = NULL;
    fp = fopen(filename, "w");
    if(fp != NULL){
        fwrite(buffers->start, 1,buffers->length, fp);
        sync();
        fclose(fp);
        return 1;
    }
    return 0;
}


static void process_image(const void *p, int size)
{
    printf("\n   /////////////////\n    PROCESSING IMAGE\n");
    printf("image size is : %d \n", size);
    char picname[100];
    sprintf(picname,"./niu_%d*%d.bmp",frame_width,frame_height);
    GenBmpFile(p,24, frame_width,frame_height,picname); 
    printf("image saved: %s \n", picname);

}

static int readFrame(void)
{
    printf("\n   /////////////////\n    READ FRAME\n");

    struct v4l2_buffer buf;
    unsigned int i;

    switch (io)
    {
    case IO_METHOD_READ:
        if (-1 == read(fd, buffers[0].start, buffers[0].length))
        {
            switch (errno)
            {
            case EAGAIN:
                return 0;

            case EIO:
                /* Could ignore EIO, see spec. */

                /* fall through */

            default:
                errno_exit("read");
            }
        }

        process_image(buffers[0].start, buffers[0].length);
        break;

    case IO_METHOD_MMAP: // default mode
        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;


        while (-1 == xioctl(fd, VIDIOC_DQBUF, &buf)) // dqbuf means take out from memory space and to process
        {
            switch (errno)
            {
            case EAGAIN:
                printf("there is no image availble yet, please wait...\r");
                usleep(5000 * 1000);
                break;
                // return 0;
            case EIO:
                break;
                /* Could ignore EIO, see spec. */
                /* fall through */
            default:
                errno_exit("VIDIOC_DQBUF");
            }
        }

        assert(buf.index < n_buffers);

        process_image(buffers[buf.index].start, buf.bytesused); // process each iamge right after getting it, then next., address of dqbuf_buff_address is related to buffers[].start

        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf)) // after processing the image data, put the buffer container back query to stream video
            errno_exit("VIDIOC_QBUF");
        break;

    case IO_METHOD_USERPTR:
        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_USERPTR;

        if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf))
        {
            switch (errno)
            {
            case EAGAIN:
                return 0;

            case EIO:
                /* Could ignore EIO, see spec. */

                /* fall through */

            default:
                errno_exit("VIDIOC_DQBUF");
            }
        }

        for (i = 0; i < n_buffers; ++i)
            if (buf.m.userptr == (unsigned long)buffers[i].start && buf.length == buffers[i].length)
                break;

        assert(i < n_buffers);

        process_image((void *)buf.m.userptr, buf.bytesused);

        if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
            errno_exit("VIDIOC_QBUF");
        break;
    }

    return 1;
}

static void enumerateMenu(unsigned long id)
{

    printf("Menu items, querymenu.name: \n");

    memset(&querymenu, 0, sizeof(querymenu));
    querymenu.id = id;

    for (querymenu.index = queryctrl.minimum;
         querymenu.index <= queryctrl.maximum;
         querymenu.index++)
    {
        if (0 == ioctl(fd, VIDIOC_QUERYMENU, &querymenu))
        {
            // printf("querymenu.name %s,  with querymenu.index: %d \n", querymenu.name, querymenu.index);
            printf(" %s ", querymenu.name);
        }
    }
}

/*
/// enumerationg all user function in old style
//  which implement all the possible user control function

memset(&queryctrl, 0, sizeof(queryctrl));

for (queryctrl.id = V4L2_CID_BASE;
     queryctrl.id < V4L2_CID_LASTP1;
     queryctrl.id++) {
    if (0 == ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl)) {
        if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
            continue;

        printf("Control %s\\n", queryctrl.name);

        if (queryctrl.type == V4L2_CTRL_TYPE_MENU)
            enumerateMenu(queryctrl.id);
    } else {
        if (errno == EINVAL)
            continue;

        perror("VIDIOC_QUERYCTRL");
        exit(EXIT_FAILURE);
    }
}

for (queryctrl.id = V4L2_CID_PRIVATE_BASE;;
     queryctrl.id++) {
    if (0 == ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl)) {
        if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED)
            continue;

        printf("Control %s\\n", queryctrl.name);

        if (queryctrl.type == V4L2_CTRL_TYPE_MENU)
            enumerateMenu(queryctrl.id);
    } else {
        if (errno == EINVAL)
            break;

        perror("VIDIOC_QUERYCTRL");
        exit(EXIT_FAILURE);
    }
}
*/

void enumerateMenuList(void)
{
    printf("\n   /////////////////\n    ENUMERATE MENU DEVICE\n");

    memset(&queryctrl, 0, sizeof(queryctrl));

    queryctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (0 == ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl))
    {
        if (!(queryctrl.flags & V4L2_CTRL_FLAG_DISABLED))
        {
            printf("-----\n VIDIOC_QUERYCTRL - Control %s\n", queryctrl.name); // list all supported control of device, like User Controls, Brightness,Saturation,Vertical Flip...

            if (queryctrl.type == V4L2_CTRL_TYPE_MENU)
                enumerateMenu(queryctrl.id); // print out all the possible type of queryctrl.name
        }

        queryctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL; // get the next property
    }
    if (errno != EINVAL)
    {
        perror("VIDIOC_QUERYCTRL");
        exit(EXIT_FAILURE);
    }
}
/*
        ===== EXAMPLES OF STDOUT =====
        -----
        Control Horizontal Flip
        -----
        Control Vertical Flip
        -----
        Control Power Line Frequency
        Menu items:
        querymenu.name Disabled,  with querymenu.index: 0 
        querymenu.name 50 Hz,  with querymenu.index: 1 
        querymenu.name 60 Hz,  with querymenu.index: 2 
        querymenu.name Auto,  with querymenu.index: 3 
        -----
        Control Sharpness
        -----
        Control Color Effects
        Menu items:
        querymenu.name None,  with querymenu.index: 0 
        querymenu.name Black & White,  with querymenu.index: 1 
        querymenu.name Sepia,  with querymenu.index: 2 
        querymenu.name Negative,  with querymenu.index: 3 

        */

// different from enumerateMenu_request_all() :: VIDIOC_QUERY_EXT_CTRL vs. VIDIOC_QUERYCTRL
// the result contains contents of enumerateMenu_request_all()
// this function result have little more options.
void enumerateExtMenuList(void)
{
    printf("\n   /////////////////\n    ENUMERATE EXT MENU DEVICE\n");

    memset(&query_ext_ctrl, 0, sizeof(query_ext_ctrl));

    query_ext_ctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    while (0 == ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &query_ext_ctrl))
    {
        if (!(query_ext_ctrl.flags & V4L2_CTRL_FLAG_DISABLED))
        {
            printf("=====\nVIDIOC_QUERY_EXT_CTRL - Control %s\n", query_ext_ctrl.name);

            if (query_ext_ctrl.type == V4L2_CTRL_TYPE_MENU)
                enumerateMenu(query_ext_ctrl.id);
        }

        query_ext_ctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND; // get the next property
    }
    if (errno != EINVAL)
    {
        perror("VIDIOC_QUERY_EXT_CTRL");
        exit(EXIT_FAILURE);
    }
}

void cameraFunctionsControlExample(void)
{

    printf("\n   /////////////////\n    CAMERA FUNCTION CONTROL\n");

    //
    // METHOD 1: a.check permission of function b.set the control id and value
    //
    memset(&queryctrl, 0, sizeof(queryctrl));
    queryctrl.id = V4L2_CID_BRIGHTNESS; // function is brightness

    if (-1 == ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl)) // if this function can not be read, then aborted.
    {
        if (errno != EINVAL)
        {
            perror("VIDIOC_QUERYCTRL");
            exit(EXIT_FAILURE);
        }
        else
        {
            printf("V4L2_CID_BRIGHTNESS is not supportedn");
        }
    }
    else if (queryctrl.flags & V4L2_CTRL_FLAG_DISABLED) // if have no permission to control, then aborted.
    {
        printf("V4L2_CID_BRIGHTNESS is not supportedn");
    }
    else // if everything is good to set,
    {
        memset(&control, 0, sizeof(control));
        control.id = V4L2_CID_BRIGHTNESS;        // set function item
        control.value = queryctrl.default_value; // set item value

        if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control)) // write and enforce control
        {
            perror("VIDIOC_S_CTRL");
            exit(EXIT_FAILURE);
        }
    }

    //
    // MEETHOD 2: just read, set the control id and value
    //
    memset(&control, 0, sizeof(control));
    control.id = V4L2_CID_CONTRAST;

    if (0 == ioctl(fd, VIDIOC_G_CTRL, &control)) // read and get control value.
    {
        control.value += 1; // add value

        /* The driver may clamp the value or return ERANGE, ignored here */

        if (-1 == ioctl(fd, VIDIOC_S_CTRL, &control) && errno != ERANGE) // set and enforce control value
        {
            perror("VIDIOC_S_CTRL");
            exit(EXIT_FAILURE);
        }
        /* Ignore if V4L2_CID_CONTRAST is unsupported */
    }
    else if (errno != EINVAL)
    {
        perror("VIDIOC_G_CTRL");
        exit(EXIT_FAILURE);
    }

    //
    // METHOD 3: only set control id and value, without checking whether it is vaild
    //
    control.id = V4L2_CID_AUDIO_MUTE;
    control.value = 1; /* silence */

    /* Errors ignored */
    ioctl(fd, VIDIOC_S_CTRL, &control);
}

// Extended Control API
void enumerateExtendedControl()
{
    printf("\n   /////////////////\n    ENUMERATE EXT CONTROL\n");

    // exactly the same code as enumerateMenuList
    // wait for implementation
    queryctrl.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    while (0 == ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl))
    {
        /* ... */
        queryctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }

    // // control class with difference
    queryctrl.id = V4L2_CTRL_CLASS_MPEG | V4L2_CTRL_FLAG_NEXT_CTRL;
    while (0 == ioctl(fd, VIDIOC_QUERYCTRL, &queryctrl))
    {
        if (V4L2_CTRL_ID2CLASS(queryctrl.id) != V4L2_CTRL_CLASS_MPEG)
            break;
        /* ... */
        queryctrl.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
}

// Camera Control reference class
// set sensor type and ...

int main(int argc, char **argv)
{
    openDevice();
    initDevice();

    // selectDevice(checkDeivce());
    // enumerateMenuList();
    // enumerateExtMenuList();
    // cameraFunctionsControlExample();
    // enumerateExtendedControl();

    startCapturing();
    readFrame();

    stopCapturing();
    uninitDevice();

    closeDevice();

    return 0;
}
