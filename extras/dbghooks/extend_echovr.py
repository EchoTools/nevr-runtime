import pefile
import os

INPUT_FILE = "echovr.exe"
OUTPUT_FILE = "echovr_patched.exe"
SECTION_SIZE = 0x6000

def align(val, align_to):
    return ((val + align_to - 1) // align_to) * align_to

pe = pefile.PE(INPUT_FILE)

# 1. Calculate New Section Offsets
# Get alignment from Optional Header
sect_align = pe.OPTIONAL_HEADER.SectionAlignment
file_align = pe.OPTIONAL_HEADER.FileAlignment

# Calculate Virtual Address (End of last section in memory, aligned)
last_sect = pe.sections[-1]
new_va = align(last_sect.VirtualAddress + last_sect.Misc_VirtualSize, sect_align)

# Calculate Raw Address (End of file, aligned)
# We use file size to safely append to the end, preserving any overlays
file_size = os.path.getsize(INPUT_FILE)
new_raw_ptr = align(file_size, file_align)

# 2. Create the Section Header
section = pefile.SectionStructure(pe.__IMAGE_SECTION_HEADER_format__)
section.__unpack__(b'\x00' * section.sizeof())
section.Name = b'.new\x00\x00\x00\x00'
section.VirtualAddress = new_va
section.Misc_VirtualSize = SECTION_SIZE
section.PointerToRawData = new_raw_ptr
section.SizeOfRawData = align(SECTION_SIZE, file_align)
section.Characteristics = 0xC0000040  # Read | Write | Initialized Data
section.PointerToRelocations = 0
section.PointerToLinenumbers = 0
section.NumberOfRelocations = 0
section.NumberOfLinenumbers = 0

# 3. Modify PE Headers
pe.sections.append(section)
pe.FILE_HEADER.NumberOfSections += 1
pe.OPTIONAL_HEADER.SizeOfImage = align(new_va + SECTION_SIZE, sect_align)

# 4. Write Header Changes
pe.write(OUTPUT_FILE)
pe.close()

# 5. Append Physical Null Bytes
# We manually append the empty space to the file on disk to match SizeOfRawData
padding_needed = new_raw_ptr - file_size
data_size = section.SizeOfRawData

with open(OUTPUT_FILE, 'ab') as f:
    f.write(b'\x00' * padding_needed)  # Align file end
    f.write(b'\x00' * data_size)       # Write section body

print(f"[OK] Added .new section to {OUTPUT_FILE}")
print(f"     VA: {hex(new_va)}, Raw Ptr: {hex(new_raw_ptr)}")
