VERT1.1

DCL IN[0], POSITION
DCL IN[1], COLOR
DCL OUT[0], POSITION
DCL OUT[1], COLOR

DCL TEMP[0]

IMM FLT32 { 1.0, 0.0, 0.0, 0.0 }
IMM FLT32 { 1.5, 0.0, 0.0, 0.0 }

ADD TEMP[0], IN[0], IMM[0]
RSQ TEMP[0].x, TEMP[0].xxxx
SUB OUT[0], TEMP[0], IMM[1]
MOV OUT[1], IN[1]

END
