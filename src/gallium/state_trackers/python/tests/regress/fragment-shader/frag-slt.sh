FRAG1.1

DCL IN[0], COLOR, LINEAR
DCL OUT[0], COLOR

DCL TEMP[0]

IMM FLT32 { 0.6, 0.6, 0.6, 0.0 }

SLT TEMP[0], IN[0], IMM[0]
MUL OUT[0], IN[0], TEMP[0]

END
