# fileserver.f
# Emulates a file server workload: create, delete, append, read, stat.

set $dir = /tmp/page-cache-deduplication/fb_test
define fileset name=bigfileset,path=$dir,size=1m,entries=100,dirwidth=10,prealloc,reuse

define process name=fileserver,instances=1
{
  thread name=fileserverthread,instances=4
  {
    flowop openfile name=open1,filesetname=bigfileset,fd=1
    flowop readwholefile name=read1,fd=1
    flowop closefile name=close1,fd=1
    flowop openfile name=open2,filesetname=bigfileset,fd=1
    flowop appendfilerand name=append1,iosize=8k,fd=1
    flowop closefile name=close2,fd=1
    flowop statfile name=stat1,filesetname=bigfileset
    flowop deletefile name=delete1,filesetname=bigfileset
    flowop createfile name=create1,filesetname=bigfileset,fd=1
    flowop closefile name=close3,fd=1
  }
}

create files
run 20
