cmd_/home/cs614/project/modules/pfns.mod := printf '%s\n'   pfns.o | awk '!x[$$0]++ { print("/home/cs614/project/modules/"$$0) }' > /home/cs614/project/modules/pfns.mod
