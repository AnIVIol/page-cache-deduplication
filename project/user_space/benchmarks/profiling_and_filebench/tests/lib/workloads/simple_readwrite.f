# simple_readwrite.f
# A basic loop of opening, reading, and appending to a single file.

set $dir = /tmp/page-cache-deduplication/fb_test
define fileset name=myfileset,path=$dir,size=10m,entries=1,dirwidth=1,prealloc,reuse

define process name=simple,instances=1
{
  thread name=worker,instances=1
  {
    flowop openfile name=open,filesetname=myfileset,fd=1
    flowop readwholefile name=read,fd=1
    flowop appendfilerand name=write,fd=1,iosize=4k
    flowop closefile name=close,fd=1
  }
}

create files
run 20
