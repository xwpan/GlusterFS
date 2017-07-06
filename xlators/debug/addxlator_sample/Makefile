TARGET = audit.so
OBJECTS = audit.o

GLUSTERFS_SRC = /home/pxw/glusterfs-3.9.1
GLUSTERFS_LIB = /usr/local/lib

HOST_OS = GF_LINUX_HOST_OS

CFLAGS = -fPIC -Wall -o0 -g \
	-DHAVE_CONFIG_H -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE \
	-D$(HOST_OS) -I$(GLUSTERFS_SRC) -I$(GLUSTERFS_SRC)/libglusterfs/src -I$(GLUSTERFS_SRC)/contrib/uuid  
      
LDFLAGS = -shared -nostartfiles -L$(GLUSTERFS_LIB) -lglusterfs -lpthread  
      
$(TARGET):$(OBJECTS)  
	$(CC) $(OBJECTS) $(LDFLAGS) -o $(TARGET)  
      
clean:  
	rm -rf $(TARGET) $(OBJECTS)  
