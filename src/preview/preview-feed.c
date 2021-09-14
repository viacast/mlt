#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>

#define METADATA_MODE "metadata"
#define FEED_MODE "feed"
#define MAX_FRAME_SIZE (1L<<22)
#define MILLI 1000

int main(int argc, char *argv[]) {
  if (argc < 3) {
    printf("Invalid arguments. Usage: previewfeed <%s|%s> \"<preview_file>\" [<fps>]\n", METADATA_MODE, FEED_MODE);
    exit(-1);
  }

  FILE *fp = 0;
  int mode = 0; // 0 -> metadata, 1 -> feed
  int fps;

  if (!strcmp(argv[1], METADATA_MODE)) {
    mode = 0;
  } else if (!strcmp(argv[1], FEED_MODE)) {
    if (argc < 4 || (fps = atoi(argv[3])) <= 0) {
      printf("Invalid usage (missing or invalid fps).\n");
      exit(-1);
    }
    mode = 1;
  } else {
    printf("Invalid mode. Supported values: '%s' or '%s'\n", METADATA_MODE, FEED_MODE);
    exit(-1);
  }

  uint32_t first_width, first_height, width, height, size;
  uint8_t data[MAX_FRAME_SIZE + 3*sizeof(uint32_t)];

  if (!mode) {
    fp = fopen(argv[2], "r");

    if (!fp) {
      printf("Failed to open file.\n");
      exit(-1);
    }

    int read = fread(data, MAX_FRAME_SIZE + 3*sizeof(uint32_t), 1, fp);
    memcpy(&width, data, sizeof(uint32_t));
    memcpy(&height, data + sizeof(uint32_t), sizeof(uint32_t));
    memcpy(&size, data + 2*sizeof(uint32_t), sizeof(uint32_t));

    printf("%"PRIu32" %"PRIu32" %"PRIu32, width, height, size);
    fclose(fp);
    exit(0);
  }

  while (1) {
    fp = fopen(argv[2], "r");
    if (!fp) {
      usleep(1000*MILLI/fps/10);
      continue;
    }
    int read = fread(data, MAX_FRAME_SIZE + 3*sizeof(uint32_t), 1, fp);
    if (!first_width) {
      memcpy(&first_width, data, sizeof(uint32_t));
      memcpy(&first_height, data + sizeof(uint32_t), sizeof(uint32_t));
      width = first_width;
      height = first_height;
    } else {
      memcpy(&width, data, sizeof(uint32_t));
      memcpy(&height, data + sizeof(uint32_t), sizeof(uint32_t));
      if (width != first_width || height != first_height) {
        fprintf(stderr, "width/height changed. exiting.\n");
        exit(-1);
      }
    }

    memcpy(&size, data + 2*sizeof(uint32_t), sizeof(uint32_t));

    int wrote = write(fileno(stdout), data + 3*sizeof(uint32_t), size*sizeof(uint8_t));
    fclose(fp);
    usleep(1000*MILLI/fps);
  }

  return 0;
}
