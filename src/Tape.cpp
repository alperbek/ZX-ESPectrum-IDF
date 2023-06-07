/*

ESPectrum, a Sinclair ZX Spectrum emulator for Espressif ESP32 SoC

Copyright (c) 2023 Víctor Iborra [Eremus] and David Crespo [dcrespo3d]
https://github.com/EremusOne/ZX-ESPectrum-IDF

Based on ZX-ESPectrum-Wiimote
Copyright (c) 2020, 2022 David Crespo [dcrespo3d]
https://github.com/dcrespo3d/ZX-ESPectrum-Wiimote

Based on previous work by Ramón Martinez and Jorge Fuertes
https://github.com/rampa069/ZX-ESPectrum

Original project by Pete Todd
https://github.com/retrogubbins/paseVGA

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

To Contact the dev team you can write to zxespectrum@gmail.com or 
visit https://zxespectrum.speccy.org/contacto

*/

#include <stdio.h>
#include <inttypes.h>
#include <string>

using namespace std;

#include "hardpins.h"
#include "FileUtils.h"
#include "CPU.h"
#include "Tape.h"
#include "Ports.h"
#include "OSDMain.h"
#include "messages.h"
#include "Z80_JLS/z80.h"

FILE *Tape::tape;
string Tape::tapeFileName = "none";
uint8_t Tape::tapeStatus = TAPE_STOPPED;
uint8_t Tape::SaveStatus = SAVE_STOPPED;
uint8_t Tape::romLoading = false;
uint8_t Tape::tapeEarBit;

static uint8_t tapeCurByte;
static uint8_t tapePhase;
static uint64_t tapeStart;
static uint32_t tapePulseCount;
static uint16_t tapeBitPulseLen;   
static uint8_t tapeBitPulseCount;     
static uint32_t tapebufByteCount;
static uint16_t tapeHdrPulses;
static uint32_t tapeBlockLen;
static size_t tapeFileSize;   
static uint8_t tapeBitMask;    
// static uint8_t tapeReadBuf[4096] = { 0 };

void Tape::Init()
{
    tape = NULL;
}

void Tape::TAP_Play()
{
   
    // TO DO: Use ram buffer (tapeReadBuf) to speed up loading
   
    switch (Tape::tapeStatus) {
    case TAPE_STOPPED:

        tape = fopen(Tape::tapeFileName.c_str(), "rb");
        if (tape == NULL)
        {
            OSD::osdCenteredMsg(OSD_TAPE_LOAD_ERR, LEVEL_ERROR);
            return;
        }
        fseek(tape,0,SEEK_END);
        tapeFileSize = ftell(tape);
        rewind (tape);

        tapePhase=TAPE_PHASE_SYNC;
        tapePulseCount=0;
        tapeEarBit=0;
        tapeBitMask=0x80;
        tapeBitPulseCount=0;
        tapeBitPulseLen=TAPE_BIT0_PULSELEN;
        tapeHdrPulses=TAPE_HDR_LONG;
        tapeBlockLen=(readByteFile(tape) | (readByteFile(tape) <<8)) + 2;
        tapeCurByte = readByteFile(tape);
        tapebufByteCount=2;
        tapeStart=CPU::global_tstates + CPU::tstates;
        Tape::tapeStatus=TAPE_LOADING;
        break;

    case TAPE_LOADING:
        Tape::tapeStatus=TAPE_PAUSED;
        break;

    case TAPE_PAUSED:
        tapeStart=CPU::global_tstates + CPU::tstates;        
        Tape::tapeStatus=TAPE_LOADING;
    }
}

void Tape::TAP_Stop()
{
    Tape::tapeStatus=TAPE_STOPPED;
    fclose(tape);
}

void Tape::TAP_Read()
{
    uint64_t tapeCurrent = (CPU::global_tstates + CPU::tstates) - tapeStart;
    
    switch (tapePhase) {
    case TAPE_PHASE_SYNC:
        if (tapeCurrent > TAPE_SYNC_LEN) {
            tapeStart=CPU::global_tstates + CPU::tstates;
            tapeEarBit ^= 1;
            tapePulseCount++;
            if (tapePulseCount>tapeHdrPulses) {
                tapePulseCount=0;
                tapePhase=TAPE_PHASE_SYNC1;
            }
        }
        break;
    case TAPE_PHASE_SYNC1:
        if (tapeCurrent > TAPE_SYNC1_LEN) {
            tapeStart=CPU::global_tstates + CPU::tstates;
            tapeEarBit ^= 1;
            tapePhase=TAPE_PHASE_SYNC2;
        }
        break;
    case TAPE_PHASE_SYNC2:
        if (tapeCurrent > TAPE_SYNC2_LEN) {
            tapeStart=CPU::global_tstates + CPU::tstates;
            tapeEarBit ^= 1;
            if (tapeCurByte & tapeBitMask) tapeBitPulseLen=TAPE_BIT1_PULSELEN; else tapeBitPulseLen=TAPE_BIT0_PULSELEN;            
            tapePhase=TAPE_PHASE_DATA;
        }
        break;
    case TAPE_PHASE_DATA:
        if (tapeCurrent > tapeBitPulseLen) {
            tapeStart=CPU::global_tstates + CPU::tstates;
            tapeEarBit ^= 1;
            tapeBitPulseCount++;
            if (tapeBitPulseCount==2) {
                tapeBitPulseCount=0;
                tapeBitMask = (tapeBitMask >>1 | tapeBitMask <<7);
                if (tapeBitMask==0x80) {
                    tapeCurByte = readByteFile(tape);
                    tapebufByteCount++;
                    if (tapebufByteCount == tapeBlockLen) {
                        tapePhase=TAPE_PHASE_PAUSE;
                        tapeEarBit=0;
                        break;
                    }
                }
                if (tapeCurByte & tapeBitMask) tapeBitPulseLen=TAPE_BIT1_PULSELEN; else tapeBitPulseLen=TAPE_BIT0_PULSELEN;
            }
        }
        break;
    case TAPE_PHASE_PAUSE:
        if (tapebufByteCount < tapeFileSize) {
            if (tapeCurrent > TAPE_BLK_PAUSELEN) {
                tapeStart=CPU::global_tstates + CPU::tstates;
                tapePulseCount=0;
                tapePhase=TAPE_PHASE_SYNC;
                tapeBlockLen+=(tapeCurByte | readByteFile(tape) <<8)+ 2;
                tapebufByteCount+=2;
                tapeCurByte=readByteFile(tape);
                if (tapeCurByte) tapeHdrPulses=TAPE_HDR_SHORT; else tapeHdrPulses=TAPE_HDR_LONG;
            }
        } else {
            Tape::tapeStatus=TAPE_STOPPED;
            fclose(tape);
        }
    } 

}

void Tape::Save() {

	FILE *fichero;
    unsigned char xxor,salir_s;
	uint8_t dato;
	int longitud;

    fichero = fopen("/sd/tap/cinta1.tap", "ab");
    if (fichero == NULL)
    {
        OSD::osdCenteredMsg(OSD_TAPE_LOAD_ERR, LEVEL_ERROR);
        return;
    }

	xxor=0;
	
	longitud=(int)(Z80::getRegDE());
	longitud+=2;
	
	dato=(uint8_t)(longitud%256);
	fprintf(fichero,"%c",dato);
	dato=(uint8_t)(longitud/256);
	fprintf(fichero,"%c",dato); // file length

	fprintf(fichero,"%c",Z80::getRegA()); // flag

	xxor^=Z80::getRegA();

	salir_s = 0;
	do {
	 	if (Z80::getRegDE() == 0)
	 		salir_s = 2;
	 	if (!salir_s) {
            dato = MemESP::readbyte(Z80::getRegIX());
			fprintf(fichero,"%c",dato);
	 		xxor^=dato;
	        Z80::setRegIX(Z80::getRegIX() + 1);
	        Z80::setRegDE(Z80::getRegDE() - 1);
	 	}
	} while (!salir_s);
	fprintf(fichero,"%c",xxor);
	Z80::setRegIX(Z80::getRegIX() + 2);

    fclose(fichero);

}
