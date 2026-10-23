// Opens the default decoder for a file's video track, decodes a few packets and
// keeps the session open for N seconds (another decoder can then be started next
// to it): `hold_probe <file> <seconds>`. Used to check that a small video doesn't
// hold the hardware decoder an 8K one needs (GitHub #437).
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc,char **argv){
 assert(argc==3);int fd=open(argv[1],O_RDONLY);assert(fd>=0);struct stat st;assert(!fstat(fd,&st));
 auto *extractor=AMediaExtractor_new();assert(AMediaExtractor_setDataSourceFd(extractor,fd,0,st.st_size)==AMEDIA_OK);
 AMediaFormat *format=nullptr;const char *mime=nullptr;
 for(size_t i=0;i<AMediaExtractor_getTrackCount(extractor);i++){
  auto *candidate=AMediaExtractor_getTrackFormat(extractor,i);
  if(AMediaFormat_getString(candidate,AMEDIAFORMAT_KEY_MIME,&mime)&&!strncmp(mime,"video/",6)){
   format=candidate;assert(AMediaExtractor_selectTrack(extractor,i)==AMEDIA_OK);break;
  }AMediaFormat_delete(candidate);
 }assert(format);
 auto *codec=AMediaCodec_createDecoderByType(mime);assert(codec);char *name=nullptr;
 assert(AMediaCodec_getName(codec,&name)==AMEDIA_OK);printf("holding %s with %s\n",mime,name);fflush(stdout);
 assert(AMediaCodec_configure(codec,format,nullptr,nullptr,0)==AMEDIA_OK);assert(AMediaCodec_start(codec)==AMEDIA_OK);
 for(int sent=0;sent<8;){ssize_t id=AMediaCodec_dequeueInputBuffer(codec,10000);if(id<0)continue;
  size_t capacity=0;auto *data=AMediaCodec_getInputBuffer(codec,id,&capacity);
  ssize_t size=AMediaExtractor_readSampleData(extractor,data,capacity);if(size<=0)break;
  AMediaCodec_queueInputBuffer(codec,id,0,size,AMediaExtractor_getSampleTime(extractor),0);
  AMediaExtractor_advance(extractor);sent++;
  AMediaCodecBufferInfo info;ssize_t out=AMediaCodec_dequeueOutputBuffer(codec,&info,1000);
  if(out>=0)AMediaCodec_releaseOutputBuffer(codec,out,false);}
 sleep(atoi(argv[2]));
 AMediaCodec_stop(codec);AMediaCodec_delete(codec);puts("released");return 0;
}
