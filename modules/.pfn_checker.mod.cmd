cmd_/home/cs614/project/modules/pfn_checker.mod := printf '%s\n'   pfn_checker.o | awk '!x[$$0]++ { print("/home/cs614/project/modules/"$$0) }' > /home/cs614/project/modules/pfn_checker.mod
