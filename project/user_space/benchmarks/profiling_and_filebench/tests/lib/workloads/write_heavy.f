# write_heavy.f
# Focuses on random writes and appends to files.
# Every write on a shared page should trigger a CoW split (kprobe + split_single_page).

set $dir = /tmp/page-cache-deduplication/fb_test
define fileset name=writeset,path=$dir,size=1m,entries=100,dirwidth=10,prealloc,reuse

define process name=writer,instances=1
{
  thread name=writerthread,instances=8
  {
    flowop openfile name=open1,filesetname=writeset,fd=1
    flowop appendfilerand name=append1,iosize=4k,fd=1
    flowop closefile name=close1,fd=1
  }
}

create files
run 30
