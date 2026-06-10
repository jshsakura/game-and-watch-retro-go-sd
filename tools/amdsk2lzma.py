#!/usr/bin/env python3
import os
import sys

def compress_lzma(data):
    import lzma

    compressed_data = lzma.compress(
        data,
        format=lzma.FORMAT_ALONE,
        filters=[
            {
                "id": lzma.FILTER_LZMA1,
                "preset": 6,
                "dict_size": 16 * 1024,
            }
        ],
    )

    compressed_data = compressed_data[13:]

    return compressed_data

def parseStandardDisk(diskBytesArray):
    if diskBytesArray[0x34] == 0x01:
        print("Disk image is aleady compressed")
        return diskBytesArray

    #header is the same
    lzmaDiskArray = bytearray(len(diskBytesArray))
    lzmaDiskArray[0:0x100] = diskBytesArray[0:0x100]
    lzmaDiskArray[0x34] = 0x01 # set compressed bit
    writeOffset = 0x100

    trackNum = diskBytesArray[0x30]
    sideNum = diskBytesArray[0x31]
    trackSize = diskBytesArray[0x32]+(diskBytesArray[0x33]<<8)-0x100 # remove header size
    offset = 0x100
    for track in range(0,trackNum):
        for side in range(0,sideNum):
            # data at offset 0x100
            compressedTrackData = compress_lzma(diskBytesArray[offset+0x100:offset+0x100+trackSize])
            lzmaDiskArray[writeOffset:writeOffset+0x100] = diskBytesArray[offset:offset+0x100] # copy header
            lzmaDiskArray[writeOffset+0xfe] = len(compressedTrackData)&0xff
            lzmaDiskArray[writeOffset+0xff] = (len(compressedTrackData)>>8)&0xff
            lzmaDiskArray[writeOffset+0x100:writeOffset+0x100+len(compressedTrackData)] = compressedTrackData
            writeOffset+=0x100+len(compressedTrackData)
            offset += 0x100 + trackSize

    return lzmaDiskArray[0:writeOffset]

def parseExtendedDisk(diskBytesArray):
    if diskBytesArray[0x32] == 0x01:
        print("Disk image is aleady compressed")
        return diskBytesArray

    lzmaDiskArray = bytearray(len(diskBytesArray))
    lzmaDiskArray[0:0x100] = diskBytesArray[0:0x100]
    lzmaDiskArray[0x32] = 0x01 # set compressed bit
    writeOffset = 0x100

    trackNum = diskBytesArray[0x30]
    sideNum = diskBytesArray[0x31]&0x03
    offsetTrackSize = 0x34
    offset = 0x100
    for track in range(0,trackNum):
        for side in range(0,sideNum):
            trackSize = (diskBytesArray[offsetTrackSize] << 8)
            offsetTrackSize += 1
            if (trackSize > 0):
                trackSize -= 0x100
                # data at offset 0x100
                compressedTrackData = compress_lzma(diskBytesArray[offset+0x100:offset+0x100+trackSize])
                lzmaDiskArray[writeOffset:writeOffset+0x100] = diskBytesArray[offset:offset+0x100] # copy header
                lzmaDiskArray[writeOffset+0xfe] = len(compressedTrackData)&0xff
                lzmaDiskArray[writeOffset+0xff] = (len(compressedTrackData)>>8)&0xff
                lzmaDiskArray[writeOffset+0x100:writeOffset+0x100+len(compressedTrackData)] = compressedTrackData
                writeOffset+=0x100+len(compressedTrackData)

                offset += 0x100 + trackSize
            else:
                print(str(track)+"/"+str(side)+"Track Size : 0")
    return lzmaDiskArray[0:writeOffset]

def analyzeDsk(dskFile,fileName):
    dskBytes = dskFile.read()
    idString = dskBytes[0:8].decode("utf-8")
    if idString == "MV - CPC":
        print("Standard Disk found")
        compressedDisk = parseStandardDisk(dskBytes)
    elif idString == "EXTENDED":
        print("Extended Disk found")
        compressedDisk = parseExtendedDisk(dskBytes)
    else:
        print("unkown disk format")
        compressedDisk = []

    return compressedDisk

n = len(sys.argv)

if n < 2: print("Usage :\ndsk2lzma.py file.dsk [compress]\n"); sys.exit(0)
if n == 2:
    compress=None
else:
    compress=sys.argv[2]

if compress != 'lzma':
    exit(0)

# Open file and find its size
print("Opening "+sys.argv[1])
dskFile = open(sys.argv[1], 'rb')
lzmaArray = analyzeDsk(dskFile,sys.argv[1])
dskFile.close()

if len(lzmaArray) > 0 :
    outFile = os.path.splitext(sys.argv[1])[0] + ".cdk"
    print("File compressed, saving "+outFile)
    newfile=open(outFile,'wb')
    newfile.write(lzmaArray)
    newfile.close()
    print("Done")
    sys.exit(0)

sys.exit(0)
