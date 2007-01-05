#!/usr/bin/python
# cvs2c
# csv converter for devices table to c table
#
# (C) Ricky White 2006 GPL V2
###############################################################################
import sys
import getopt
import csv
from optparse import OptionParser



def main():
	usage = "usage: %prog [options] arg";
	parser = OptionParser(usage)
	
	parser.add_option("-i", "--input-file", type="string", dest="infile")
	parser.add_option("-o", "--output-file", type="string", dest="outfile")
	(options, args) = parser.parse_args()


	cfile = open(options.outfile,"wb")
	cfile.writelines(
	"///////////////////////////////////////////////////////////////////////////////////////////////\n" \
	"//\n" \
	"//		**** DO NOT EDIT ***\n" \
	"// Automatically generated from "+options.infile+"\n" \
	"// You should edit the ods file used to generate the above csv file for perminant changes\n" \
	"//\n" \
	"//////////////////////////////////////////////////////////////////////////////////////////////\n\n")
	cfile.writelines("#include \"devices.h\"\n")
	cfile.writelines("DEVICE devices[] =\n")
	reader = csv.reader( open(options.infile, "rb") )
	i = 0;
	for row in reader:
		# Skip lines marked for exclusion
		if row[0][0]!='#':
			if i==0:
				cfile.writelines("\t{{\n");
			else:
				cfile.writelines(",\n\t{\n");
			if row[22]!="":
				cfile.writelines("\t\t// "+row[22]+"\n")
			cfile.writelines("\t\t\""+row[0]+"\",\t// Device Name\n")
			cfile.writelines("\t\t"+row[1]+",\t\t\t// Device id (Family)\n")
			cfile.writelines("\t\t"+row[2]+",\t\t\t// Device unique id\n")
			cfile.writelines("\t\t"+row[3]+",\t\t\t// Device revision (-1 = any match\n")
			cfile.writelines("\t\t"+row[5]+",\t\t\t// Flash Size (Marketing number)\n")
			cfile.writelines("\t\t"+row[6]+",\t\t\t// Flash Sector Size\n")
			cfile.writelines("\t\t"+row[7]+",\t\t\t// XRAM Size\n")
			cfile.writelines("\t\t"+row[8]+",\t\t\t// Has External Bus\n")
			cfile.writelines("\t\t"+row[9]+",\t\t\t// Tested by ec2drv developers\n")
			cfile.writelines("\t\t"+row[10]+",\t\t\t// Flash lock type\n")
			cfile.writelines("\t\t"+row[11]+",\t\t\t// Read lock\n")
			cfile.writelines("\t\t"+row[12]+",\t\t\t// Write lock\n")
			cfile.writelines("\t\t"+row[13]+",\t\t\t// Single lock\n")
			cfile.writelines("\t\t"+row[14]+",\t\t\t// Reserved Flash bottom addr\n")
			cfile.writelines("\t\t"+row[15]+",\t\t\t// Reserved Flash top addr\n")
			cfile.writelines("\t\t"+row[16]+",\t\t\t// Has flash scratchpad (SFLE)\n")
			cfile.writelines("\t\t"+row[17]+",\t\t\t// Scratch start addr (SFLE=1)\n")
			cfile.writelines("\t\t"+row[18]+",\t\t\t// Scratchpad length (bytes)\n")
			cfile.writelines("\t\t"+row[19]+",\t\t\t// Scratchpad sector size (bytes)\n")
			cfile.writelines("\t\t"+row[20]+",\t\t\t// Has Pages SFR registers\n")
			cfile.writelines("\t\t"+row[21]+",\t\t\t// USB FIFO size\n")
			cfile.writelines("\t\t"+row[22]+",\t\t\t// Debug Mode (JTAG /C2)\n")
			cfile.writelines("\t}")
			i+=1
	
	cfile.writelines(",\n\t{0}}\n")
	cfile.writelines(";\n")
	cfile.close()


#   	parser.error("options -a and -b are mutually exclusive");
if __name__ == "__main__":
    main()
	





