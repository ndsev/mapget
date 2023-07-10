!#/usr/bin/env bash

# Define header of the markdown table
echo "| Filename | Unzipped Size | Zipped Size |" > table.md
echo "|----------|--------------:|------------:|" >> table.md

# Loop over each file in the directory
for file in tile.*; do
    # Zip the file
    zip "${file}.zip" "$file"

    # Get the unzipped and zipped sizes in bytes
    unzipped_size=$(stat -c%s "$file")
    zipped_size=$(stat -c%s "${file}.zip")

    # Convert the sizes to human-readable format (kB, MB, etc.)
    unzipped_size_human=$(numfmt --to=iec-i --suffix=B "$unzipped_size")
    zipped_size_human=$(numfmt --to=iec-i --suffix=B "$zipped_size")

    # Add a row to the markdown table
    echo "| $file | $unzipped_size_human | $zipped_size_human |" >> table.md
done

