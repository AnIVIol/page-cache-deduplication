cmd_/home/cs614/project/modules/check_pfn.mod := printf '%s\n'   check_pfn.o | awk '!x[$$0]++ { print("/home/cs614/project/modules/"$$0) }' > /home/cs614/project/modules/check_pfn.mod
