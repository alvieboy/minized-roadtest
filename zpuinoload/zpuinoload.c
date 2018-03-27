#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <string.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>

#define ZPU_IOCTL_SETRESET _IOW('Z', 0, unsigned)

#define SKETCH_SIGNATURE 0x310AFADE
#define SKETCH_BOARD     0xBC010000
#define SKETCH_OFFSET    0x1008

int main(int argc, char **argv)
{
        uint32_t v;
        int r, sketchfd, drvfd;
        unsigned aligned_sketch_size;

        if (argc<2)
                return -1;

        sketchfd = open(argv[1], O_RDONLY);
        if (sketchfd<0) {
                perror("cannot open");
                return -1;
        }
        if (read(sketchfd,&v,sizeof(v))!=sizeof(v)) {
                perror("read");
                close(sketchfd);
                return -1;
        }
        if ( htobe32(v) != SKETCH_SIGNATURE)  {
                fprintf(stderr,"Invalid signature %08x\n", htobe32(v));
                close(sketchfd);
                return -1;
        }

        if (read(sketchfd,&v,sizeof(v))!=sizeof(v)) {
                perror("read");
                close(sketchfd);
                return -1;
        }
        if ( htobe32(v) != SKETCH_BOARD)  {
                fprintf(stderr,"Invalid board %08x\n", htobe32(v));
                close(sketchfd);
                return -1;
        }
        // Ready to go. Get sketch size
        off_t sketch_size = lseek(sketchfd, 0, SEEK_END) - 8;

        if (sketch_size<0) {
                perror("llseek");
                close(sketchfd);
                return -1;
        }

        if (lseek(sketchfd,8,SEEK_SET)!=8) {
                perror("llseek");
                close(sketchfd);
                return -1;
        }
        // Align sketch size
        aligned_sketch_size = (sketch_size + 3) & ~3;
        // Alloc and load
        uint32_t *sketchdata = (uint32_t*)malloc(aligned_sketch_size);
        if (sketchdata==NULL) {
                fprintf(stderr,"Cannot allocate memory: %s\n", strerror(errno));
                close(sketchfd);
                return -1;
        }
        // Load sketch into memory
        r = read(sketchfd, sketchdata, sketch_size);

        if (r!=sketch_size) {
                fprintf(stderr,"Short read, want %d (aligned %d) got %d: %s\n",
                        sketch_size,
                        aligned_sketch_size,
                        r,
                        strerror(errno));
                close(sketchfd);
                return -1;
        }
        close(sketchfd);
        // Swap endianess
        {
                unsigned words = aligned_sketch_size>>2;
                uint32_t *ptr = sketchdata;
                while (words--) {
                        *ptr = __bswap_32(*ptr);
                        ptr++;
                }
        }

        drvfd = open("/dev/zpuinodrv", O_RDWR);

        if (drvfd<0) {
                perror("cannot open zpuinodrv");
                return -1;
        }

        if (ioctl(drvfd, ZPU_IOCTL_SETRESET, 1)<0) {
                perror("ioctl");
                close(drvfd);
                return -1;
        }


        // Write sketch
        if (lseek(drvfd, SKETCH_OFFSET, SEEK_SET)!=SKETCH_OFFSET) {
                fprintf(stderr,"Cannot seek: %s\n", strerror(errno));
                close(drvfd);
                return -1;
        }
        if (write(drvfd, sketchdata, aligned_sketch_size)!=aligned_sketch_size) {
                fprintf(stderr,"Short write: %s\n", strerror(errno));
                close(drvfd);
                return -1;
        }
        printf("Removing reset.\n");
        if (ioctl(drvfd, ZPU_IOCTL_SETRESET, 0)<0) {
                perror("ioctl");
                close(drvfd);
                return -1;
        }

        close(drvfd);
        return 0;
}
