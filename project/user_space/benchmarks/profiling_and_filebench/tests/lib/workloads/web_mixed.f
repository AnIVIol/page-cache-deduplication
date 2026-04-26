# web_mixed.f
# Mixed workload: 80% Read, 20% Write.
# Simulates a typical web server environment.

set $dir = /tmp/page-cache-deduplication/fb_test
define fileset name=webset,path=$dir,size=1m,entries=100,dirwidth=10,prealloc,reuse

define process name=webserver,instances=1
{
  thread name=webthread,instances=8
  {
    flowop openfile name=open1,filesetname=webset,fd=1
    flowop readwholefile name=read1,fd=1
    flowop closefile name=close1,fd=1
    flowop openfile name=open2,filesetname=webset,fd=1
    flowop appendfilerand name=append1,iosize=8k,fd=1
    flowop closefile name=close2,fd=1
  }
}

create files
run 30
