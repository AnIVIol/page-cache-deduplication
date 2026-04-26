Getting started:

Let’s say we are in the project directory
cd modules
 ./ok.sh scanner pagecache_inspector (this script rebuilds and inserts the modules)
cd ../user_space/manual_testing/


./generate_files <num_files> <number_of_pages_in_each_file> (every page created is the same and has been filled with strings of “AAAAAAA\n”)
echo "1" | sudo tee /sys/kernel/dedup_scanner/run
echo <file_path> | sudo tee /sys/kernel/dedup_scanner/scan_file
echo <file_path> | sudo tee /sys/kernel/pagecache_inspector/filename_print  (prints details about file’s index, it’s pfn and refcount)
./write_offset <file_path> <offset_to_write_at> <content>
./write_test <file_path> <string> (prints the offset, character, other than those included in the string, use “A\n” if files generated using ./generate_files)
You can use cat, echo or editors like vi to write/verify the contents as well. Some delay will be observed while exiting the vi editor if the file opened is currently deduplicated.
echo 3, truncate, symlink, and cp can all be tested directly using the standard commands.
