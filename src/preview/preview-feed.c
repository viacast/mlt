#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>

#define METADATA_MODE "metadata"
#define FEED_MODE "feed"
#define MAX_FRAME_SIZE 1<<22
#define MILLI 1000

int main(int argc, char *argv[]) {
  if (argc < 3) {
    printf("Invalid arguments. Usage: previewfeed <%s|%s> \"<preview_file>\" [<fps>]\n", METADATA_MODE, FEED_MODE);
    exit(-1);
  }

  FILE *fp = 0;
  int mode; // 0 -> metadata, 1 -> feed
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

  uint32_t width, height, size;

  if (!mode) {
    fp = fopen(argv[2], "rb");

    if (!fp) {
      printf("Failed to open file.\n");
      exit(-1);
    }

    fread(&width, sizeof(uint32_t), 1, fp);
    fread(&height, sizeof(uint32_t), 1, fp);
    fread(&size, sizeof(uint32_t), 1, fp);

    printf("%"PRIu32" %"PRIu32" %"PRIu32, width, height, size);
    fclose(fp);
    exit(0);
  }

  uint8_t frame[MAX_FRAME_SIZE];
  fp = fopen(argv[2], "rb");

  while (1) {
    if (!fp) {
      fp = fopen(argv[2], "rb");
      sleep(1);
      continue;
    }

    fread(&width, sizeof(uint32_t), 1, fp);
    fread(&height, sizeof(uint32_t), 1, fp);
    fread(&size, sizeof(uint32_t), 1, fp);
    fread(frame, sizeof(uint8_t), size, fp);

    write(fileno(stdout), frame, size*sizeof(uint8_t));

    fclose(fp);
    fp = 0;
    usleep(1000*MILLI/fps);
  }

  fclose(fp);    

  return 0;
}
