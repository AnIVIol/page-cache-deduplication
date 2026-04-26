# read_heavy.f
# Focuses on intensive read operations from multiple threads.
# Shared pages should increase CPU cache hits.

set $dir = /tmp/page-cache-deduplication/fb_test
define fileset name=readset,path=$dir,size=1m,entries=100,dirwidth=10,prealloc,reuse

define process name=reader,instances=1
{
  thread name=readerthread,instances=8
  {
    flowop openfile name=open1,filesetname=readset,fd=1
    flowop readwholefile name=read1,fd=1
    flowop closefile name=close1,fd=1
  }
}

create files
run 30
