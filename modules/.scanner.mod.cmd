cmd_/home/cs614/project/modules/scanner.mod := printf '%s\n'   scanner.o | awk '!x[$$0]++ { print("/home/cs614/project/modules/"$$0) }' > /home/cs614/project/modules/scanner.mod
