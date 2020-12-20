# LITTLEFS File System

The NodeMCU project uses the [LITTLEFS](https://github.com/littlefs-project/littlefs)
filesystem to store files in the flash chip. The technical details about how this is configured can be found below, along with various build time options.



# Technical Details

One of the advantages of LITTLEFS is that formatting the filesystem is very fast (unlike SPIFFS). However, if you want to limit the size of the
filesystems then just place the following define in `user_config.h` or some other file that is included during the build.

```
#define LITTLEFS_MAX_FILESYSTEM_SIZE	32768
```

