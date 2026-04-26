# varmail.f
# Emulates a mail server: small files, high frequency syncs.

set $dir = /tmp/page-cache-deduplication/fb_test
define fileset name=mailfileset,path=$dir,size=16k,entries=1000,dirwidth=20,prealloc,reuse

define process name=varmail,instances=1
{
  thread name=varmailthread,instances=16
  {
    flowop openfile name=open1,filesetname=mailfileset,fd=1
    flowop appendfilerand name=append1,iosize=8k,fd=1
    flowop closefile name=close1,fd=1
    flowop readwholefile name=read1,filesetname=mailfileset,fd=1
    flowop closefile name=close2,fd=1
    flowop deletefile name=delete1,filesetname=mailfileset
    flowop createfile name=create1,filesetname=mailfileset,fd=1
    flowop closefile name=close3,fd=1
    flowop sync name=sync1
  }
}

create files
run 20
