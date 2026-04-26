cmd_/home/cs614/project/modules/pfn_tracker.mod := printf '%s\n'   pfn_tracker.o | awk '!x[$$0]++ { print("/home/cs614/project/modules/"$$0) }' > /home/cs614/project/modules/pfn_tracker.mod
