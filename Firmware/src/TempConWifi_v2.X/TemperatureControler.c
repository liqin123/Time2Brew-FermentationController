#include <p33Exxxx.h>
#include <stdio.h>
#include <string.h>
#include "FlashFS.h"
#include "TemperatureController.h"
#include "SystemConfiguration.h"
#include "OneWireTemperature.h"
#include "Pack.h"
#include "Settings.h"
#include "PID.h"
#include "http_server.h"
#include "RTC.h"
#include "Http_API.h"
#include "fletcherChecksum.h"
#include "RLE_Compressor.h"


MACHINE_STATE globalstate;
//char scratch[256];

char isTemperatureController_Initialized = 0;
char suspendTempCon = 0;

//BYTE trend_RLE_SampleBuffer[6];

RECOVERY_RECORD recovery_record;

int GetProfileName(const char *profileID, char *ProfileName) {
    char filename[64];
    ff_File handle;
    int res;
    int bw;

    sprintf(filename, "prfl.%s", profileID);
    res = ff_OpenByFileName(&handle, filename, 0);
    if (res != FR_OK) return res;
    res = ff_Read(&handle, (BYTE *) ProfileName, 64, &bw);
    if (res != FR_OK) return res;
    return FR_OK;
}

float rawReadTemp(int ProbeIDX) {
    float ret;

    union {
        int i;
        unsigned char ub[2];
    } temp;

    int retry = 3;
    ret = -32767;
    while (retry--) {
        if (OneWireIsBusShorted()) continue;
        if (OneWireReset(ProbeIDX) != 0) continue;
        OneWireWriteByte(ProbeIDX, 0xCC);
        OneWireWriteByte(ProbeIDX, 0xBE);
        if (OneWireReadByte(ProbeIDX, &temp.ub[0]) != 1) continue;
        if (OneWireReadByte(ProbeIDX, &temp.ub[1]) != 1) continue;
        ret = ((float) temp.i * 0.0625);
        if (ret < 1250 && ret>-550) break;
        ret = -32767;
    }

    //Send the command to start conversion...
    OneWireReset(ProbeIDX);
    OneWireWriteByte(ProbeIDX, 0xCC);
    OneWireWriteByte(ProbeIDX, 0x44);
    return ret;
}

int ReadTemperature(MACHINE_STATE *dest) {

    switch (dest->equipmentConfig.Probe0Assignment) {
        case 1:
            dest->ProcessTemperature = rawReadTemp(0);
            break;
        case 2:
            dest->TargetTemperature = rawReadTemp(0);
    }

    switch (dest->equipmentConfig.Probe1Assignment) {
        case 1:
            dest->ProcessTemperature = rawReadTemp(1);
            break;
        case 2:
            dest->TargetTemperature = rawReadTemp(1);
    }

    return 1;
}

int LoadProfileInstance(unsigned int ProfileID, unsigned long instance, char *msg, MACHINE_STATE *dest) {
    PROFILE_STEP step;
    char strProfileID[7];
    char FileName[64];
    int bytesRead;
    ff_File *handle = &dest->Profile;
    int res;

    sprintf(strProfileID, "%u", ProfileID);
    sprintf(FileName, "inst.%u.%lu", ProfileID, instance);

    Log("Loading Profile: \"%s\"\r\n", FileName);

    res = ff_OpenByFileName(handle, FileName, 0);
    if (res != FR_OK) {
        sprintf(msg, "LoadProfile Error: f_open res=%s", Translate_DRESULT(res));
        return res;
    }

    res = GetProfileName(strProfileID, dest->ActiveProfileName);
    if (res != FR_OK) {
        sprintf(msg, "Unable to Get Profile Name res=%s", Translate_DRESULT(res));
        return res;
    }

    dest->totalProfileRunTime = 0;
    dest->StepCount = 0;
    int stepIdx = 0;
    while ((handle->Length - handle->Position) >= sizeof (PROFILE_STEP)) {
        res = ff_Read(handle, (BYTE *) & step, sizeof (PROFILE_STEP), &bytesRead);
        if (res != FR_OK) {
            sprintf(msg, "LoadProfile Error: StepIdx=%d f_read res=%s", stepIdx, Translate_DRESULT(res));
            return res;
        }

        Log(" -StepIdx:%i,%f2,%f2,%ul\r\n", stepIdx, (double) step.StartTemperature, (double) step.EndTemperature, step.Duration);

        if (step.StartTemperature > 125.0 || step.StartTemperature<-25.0) {
            sprintf(msg, "LoadProfile Error: StepIdx=%i startTemp is out of bounds = %f", stepIdx, (double) step.StartTemperature);
            return FR_ERR_OTHER;
        }

        if (step.EndTemperature > 125.0 || step.EndTemperature<-25.0) {
            sprintf(msg, "LoadProfile Error: StepIdx=%i endTemp is out of bounds = %f", stepIdx, (double) step.EndTemperature);
            return FR_ERR_OTHER;
        }

        dest->totalProfileRunTime += step.Duration;
        stepIdx++;
    }
    dest->StepCount = stepIdx;
    dest->ProfileID = ProfileID;
    ff_Seek(handle, 0);
    Log("Done\r\n");
    return FR_OK;
}

int SetEquipmentProfile(unsigned int ID, char *msg, MACHINE_STATE *dest) {
    char filename[64];
    sprintf(filename, "equip.%u", ID);
    EQUIPMENT_PROFILE temp;
    int res;
    res = LoadEquipmentProfile(filename, msg, &temp);
    if (res != FR_OK) return res;

    DISABLE_INTERRUPTS;
    memcpy((BYTE*) & dest->equipmentConfig, (BYTE *) & temp, sizeof (EQUIPMENT_PROFILE));

    PID_SetTunings(&dest->TargetPID,
            dest->equipmentConfig.Target_Kp,
            dest->equipmentConfig.Target_Ki,
            dest->equipmentConfig.Target_Kd,
            dest->equipmentConfig.Target_D_FilterGain,
            dest->equipmentConfig.Target_D_FilterCoeff,
            dest->equipmentConfig.Target_D_AdaptiveBand);

    PID_SetTunings(&dest->ProcessPID,
            dest->equipmentConfig.Process_Kp,
            dest->equipmentConfig.Process_Ki,
            dest->equipmentConfig.Process_Kd,
            dest->equipmentConfig.Process_D_FilterGain,
            dest->equipmentConfig.Process_D_FilterCoeff,
            dest->equipmentConfig.Process_D_AdaptiveBand);

    if (dest->equipmentConfig.RegulationMode == REGULATIONMODE_ComplexThreshold) {

    } else {
        PID_SetOutputLimits(&dest->TargetPID, -1, 1);
        PID_SetOutputLimits(&dest->ProcessPID, -1, 1);
    }


    dest->equipmentProfileID = ID;

    ENABLE_INTERRUPTS;
    return FR_OK;
}

int LoadEquipmentProfile(const char *FileName, char *msg, EQUIPMENT_PROFILE *dest) {
    int bRead;
    ff_File Handle;
    ff_File *ptrHandle = &Handle;
    int res;

    res = ff_OpenByFileName(ptrHandle, FileName, 0);
    if (res != FR_OK) {
        sprintf(msg, "Error: Unable to open Equipment Profile: '%s' | res='%s'", FileName, Translate_DRESULT(res));
        return res;
    }

    res = ff_Read(ptrHandle, (BYTE *) dest, sizeof (EQUIPMENT_PROFILE), &bRead);
    if (res != FR_OK) {
        sprintf(msg, "Error: Unable to Read Equipment Profile: '%s' | res='%s'", FileName, Translate_DRESULT(res));
        return res;
    }

    uint16_t chksum = fletcher16((BYTE *) dest, sizeof (EQUIPMENT_PROFILE) - 2); //Checksum all but the last two bytes...

    if (dest->CheckSum != chksum) {
        sprintf(msg, "Error: Checksum Mismatch! Expected=%d Actual=%d", chksum, dest->CheckSum);
        return FR_ERR_OTHER;
    }

    if (dest->Probe0Assignment > 2 || dest->Probe1Assignment > 2) {
        sprintf(msg, "Error: Probe Assignment(s) are out of bounds!\r\n");
        return FR_ERR_OTHER;
    }

    if (dest->TargetOutput_Max > 1250 || dest->TargetOutput_Max<-250) {
        sprintf(msg, "Error: TargetOutput_Max is out of bounds!\r\n");
        return FR_ERR_OTHER;
    }

    if (dest->TargetOutput_Min > 1250 || dest->TargetOutput_Min<-250) {
        sprintf(msg, "Error: TargetOutput_Max is out of bounds!\r\n");
        return FR_ERR_OTHER;
    }

    if (dest->TargetOutput_Min > dest->TargetOutput_Max) {
        sprintf(msg, "Error: TargetOutput_Min is greater than dest->TargetOutput_Max!\r\n");
        return FR_ERR_OTHER;
    }

    return FR_OK;
}

void WriteRecoveryRecord(MACHINE_STATE *source) {
    DISABLE_INTERRUPTS;
    recovery_record.CoolWhenCanTurnOff = source->CoolWhenCanTurnOff;
    recovery_record.CoolWhenCanTurnOn = source->CoolWhenCanTurnOn;
    recovery_record.HeatWhenCanTurnOff = source->HeatWhenCanTurnOff;
    recovery_record.HeatWhenCanTurnOn = source->HeatWhenCanTurnOn;
    recovery_record.TargetPID_Integral = source->TargetPID.ITerm;
    recovery_record.ProcessPID_Integral = source->ProcessPID.ITerm;
    ENABLE_INTERRUPTS;

    recovery_record.ProfileID = source->ProfileID;
    recovery_record.EquipmentID = source->equipmentProfileID;
    recovery_record.ManualSetPoint = source->ManualSetPoint;
    recovery_record.ProfileStartTime = source->ProfileStartTime;
    recovery_record.SystemMode = source->SystemMode;

    nvsRAM_Write((BYTE*) & recovery_record, 0, sizeof (recovery_record));

    uint16_t calcChecksum = fletcher16((BYTE*) & recovery_record, sizeof (recovery_record));
    nvsRAM_Write((BYTE*) & calcChecksum, sizeof (recovery_record), 2);

}

void InitializeRecoveryRecord() {
    char scratch[512];
    EQUIPMENT_PROFILE dummy;
    int res;
    res = LoadEquipmentProfile("equip.0", scratch, &dummy);
    if (res != FR_OK) {
        CreateDefaultEquipmentProfile();
        res = LoadEquipmentProfile("equip.0", scratch, &dummy);
        if (res != FR_OK) {
            Log("Error Opening Default Equipment Config; Err='%s' Msg='%s'", Translate_DRESULT(res), scratch);
            while (1);
        }
    }

    recovery_record.CoolWhenCanTurnOff = 0;
    recovery_record.CoolWhenCanTurnOn = 0;
    recovery_record.EquipmentID = 0;
    recovery_record.HeatWhenCanTurnOff = 0;
    recovery_record.HeatWhenCanTurnOn = 0;
    recovery_record.ManualSetPoint = 0;
    recovery_record.ProfileStartTime = 0;
    recovery_record.ProfileID = 0;
    recovery_record.SystemMode = SYSTEMMODE_IDLE;
    recovery_record.TargetPID_Integral = 0;
    recovery_record.ProcessPID_Integral = 0;
    Log("----------------------\r\nInitialized Recovery Record\r\n---------------------------------------\r\n");
    Log(".CoolWhenCanTurnOff=%ul\r\n", recovery_record.CoolWhenCanTurnOff);
    Log(".CoolWhenCanTurnOn=%ul\r\n", recovery_record.CoolWhenCanTurnOn);
    Log(".EquipmentID=%xi\r\n", recovery_record.EquipmentID);
    Log(".HeatWhenCanTurnOff=%ul\r\n", recovery_record.HeatWhenCanTurnOff);
    Log(".HeatWhenCanTurnOn=%ul\r\n", recovery_record.HeatWhenCanTurnOn);
    Log(".ManualSetPoint=%f2\r\n", recovery_record.ManualSetPoint);
    Log(".ProfileStartTime=%ul\r\n", recovery_record.ProfileStartTime);
    Log(".SystemMode=%ub\r\n", recovery_record.SystemMode);
    Log(".ProfileID=%d\r\n\r\n", recovery_record.ProfileID);
    nvsRAM_Write((BYTE*) & recovery_record, 0, sizeof (recovery_record));

    uint16_t calcChecksum = fletcher16((BYTE*) & recovery_record, sizeof (recovery_record));
    nvsRAM_Write((BYTE*) & calcChecksum, sizeof (recovery_record), 2);
}

int RestoreRecoveryRecord() {
    char scratch[256];
    char strProfileID[7];
    int res;
    MACHINE_STATE temp;
    uint16_t recordChecksum, calcChecksum;
    Log("---Reading Recovery Record\r\n");
    nvsRAM_Read((BYTE *) & recovery_record, 0, sizeof (recovery_record));
    nvsRAM_Read((BYTE *) & recordChecksum, sizeof (recovery_record), 2);
    calcChecksum = fletcher16((BYTE *) & recovery_record, sizeof (recovery_record));

    if (calcChecksum != recordChecksum) {
        Log("Bad Checksum Record=%xi Calculated=%xi\r\n", recordChecksum, calcChecksum);
        return 0;
    }

    Log("   .CoolWhenCanTurnOff=%ul\r\n", recovery_record.CoolWhenCanTurnOff);
    Log("   .CoolWhenCanTurnOn=%ul\r\n", recovery_record.CoolWhenCanTurnOn);
    Log("   .EquipmentID=%ui\r\n", recovery_record.EquipmentID);
    Log("   .HeatWhenCanTurnOff=%ul\r\n", recovery_record.HeatWhenCanTurnOff);
    Log("   .HeatWhenCanTurnOn=%ul\r\n", recovery_record.HeatWhenCanTurnOn);
    Log("   .ManualSetPoint=%f2\r\n", recovery_record.ManualSetPoint);
    Log("   .ProfileStartTime=%ul\r\n", recovery_record.ProfileStartTime);
    Log("   .SystemMode=%ub\r\n", recovery_record.SystemMode);
    Log("   .ProfileID=%ui\r\n", recovery_record.ProfileID);

    sprintf(strProfileID, "%u", recovery_record.ProfileID);

    //Restore the Profile and Equipment Names

    if (recovery_record.SystemMode == SYSTEMMODE_Profile) {

        Log("Loading Profile Instance...");
        res = LoadProfileInstance(recovery_record.ProfileID, recovery_record.ProfileStartTime, scratch, &temp);
        if (res != FR_OK) {
            Log("FAIL: msg=%s\r\n", scratch);
            return 0;
        } else {
            Log("OK\r\n");
        }

        sprintf(scratch, "trnd.%u.%lu", recovery_record.ProfileID, recovery_record.ProfileStartTime);
        Log("Opening Trend Record '%s'...", scratch);

        unsigned long ProfileElapsedSeconds = RTC_GetTime() - recovery_record.ProfileStartTime;
        ProfileElapsedSeconds /= 60;

        //res = RLE_Open(&temp.trend_RLE_State, scratch, &temp.trend_RLE_FileHandle, trend_RLE_SampleBuffer, 6, ProfileElapsedSeconds, sizeof (RECOVERY_RECORD) + 4);
        res = ff_OpenByFileName(&temp.trendFile, scratch, 0);
        if (res != FR_OK) {
            Log("FAIL: res=%s\r\n", Translate_DRESULT(res));
            return 0;
        }
        //Log("Origin Sector=%xl\r\n", temp.TrendRecord.OriginSector);
    } else {
        temp.ActiveProfileName[0] = 0;
    }


    //Load the Equipment Profile
    Log("Loading Equipment Profile...");
    sprintf(scratch, "equip.%u", recovery_record.EquipmentID);
    res = SetEquipmentProfile(recovery_record.EquipmentID, scratch, &temp);
    if (res != FR_OK) {
        Log("FAIL: msg=%s\r\n", scratch);
        return 0;
    } else {
        Log("OK\r\n");
    }

    //Restore the other settings
    temp.CoolWhenCanTurnOff = recovery_record.CoolWhenCanTurnOff;
    temp.CoolWhenCanTurnOn = recovery_record.CoolWhenCanTurnOn;
    temp.HeatWhenCanTurnOff = recovery_record.HeatWhenCanTurnOff;
    temp.HeatWhenCanTurnOn = recovery_record.HeatWhenCanTurnOn;
    temp.ManualSetPoint = recovery_record.ManualSetPoint;
    temp.ProfileStartTime = recovery_record.ProfileStartTime;
    temp.SystemMode = recovery_record.SystemMode;

    //Ensure that the relays are both off...
    temp.CoolRelay = 0;
    temp.HeatRelay = 0;
    //And the duty cycle timer is zero as well...
    temp.TimeTurnedOn = 0;

    //Get an initial temperature for the PID loops to work on
    ReadTemperature(&temp);
    Delay(1);
    ReadTemperature(&temp);

    //And initialize them so they are consistent...
    //the output and input values are required.
    PID_Initialize(&temp.ProcessPID, recovery_record.ProcessPID_Integral);
    PID_Initialize(&temp.TargetPID, recovery_record.TargetPID_Integral);

    //Make it active!
    DISABLE_INTERRUPTS;
    memcpy((BYTE *) & globalstate, (BYTE *) & temp, sizeof (MACHINE_STATE));
    ENABLE_INTERRUPTS;
    Log("Recovery Success!\r\n");

    return 1;
}

int ExecuteProfile(unsigned int ProfileID, char *msg) {
    int res;
    unsigned long StartTime;
    char templateFilename[64];
    char instanceFilename[64];
    char trendFilename[64];
    char strProfileID[7];
    char scratch[256];

    DISABLE_INTERRUPTS;
    StartTime = globalstate.SystemTime;
    ENABLE_INTERRUPTS;

    sprintf(strProfileID, "%u", ProfileID);
    sprintf(instanceFilename, "inst.%u.%lu", ProfileID, StartTime);
    sprintf(templateFilename, "prfl.%u", ProfileID);
    sprintf(trendFilename, "trnd.%u.%lu", ProfileID, StartTime);

    //Create the Profile Instance
    Log("Create Profile Instance...");
    res = ff_copy(templateFilename, 64, instanceFilename, 0, 0, 1);
    if (res != FR_OK) {
        sprintf(msg, "Error: unable to allocate profile trend record='%s' res=%s", trendFilename, Translate_DRESULT(res));
        return -1;
    } else {
        Log("OK\r\n");
    }


    Log("Creating Trend File...");
    ff_Delete(trendFilename);
    //res = RLE_CreateNew(&globalstate.trend_RLE_State, trendFilename, &globalstate.trend_RLE_FileHandle, trend_RLE_SampleBuffer, 6, sizeof (RECOVERY_RECORD) + 4);
    res = ff_OpenByFileName(&globalstate.trendFile, trendFilename, 1);
    if (res != FR_OK) {
        sprintf(msg, "Error: unable to allocate profile trend record='%s' res=%s", trendFilename, Translate_DRESULT(res));
        return -1;
    } else {
        Log("OK\r\n");
    }

    Log("Loading Instance into Machine State...");
    res = LoadProfileInstance(ProfileID, StartTime, scratch, &globalstate);
    if (res != FR_OK) {
        sprintf(msg, "Error: unable to load Instance. Msg='%s'", scratch);
        return -1;
    } else {
        Log("OK\r\n");
    }

    BuildProfileInstanceListing(strProfileID);

    globalstate.ProfileStartTime = StartTime;
    PID_Initialize(&globalstate.ProcessPID, 0);
    PID_Initialize(&globalstate.TargetPID, 0);

    DISABLE_INTERRUPTS;
    globalstate.SystemMode = SYSTEMMODE_Profile;
    ENABLE_INTERRUPTS;

    WriteRecoveryRecord(&globalstate);

    return 1;
}

int TerminateProfile(char *msg) {
    char scratch[sizeof (PROFILE_STEP)];
    int bw;
    BYTE OriginalMode = globalstate.SystemMode;
    PROFILE_STEP current;

    DISABLE_INTERRUPTS;
    globalstate.SystemMode = SYSTEMMODE_IDLE;
    ENABLE_INTERRUPTS;

    ff_File *existing = &globalstate.Profile;

    ff_Seek(existing, 0);

    if (OriginalMode == SYSTEMMODE_Profile) {
        ff_File newProfile;
        ff_Delete("tempprofile");
        ff_OpenByFileName(&newProfile, "tempprofile", 1);
        int idx;
        for (idx = 0; idx < globalstate.StepIdx; idx++) {
            ff_Read(existing, (BYTE*) scratch, sizeof (PROFILE_STEP), &bw);
            ff_Append(&newProfile, (BYTE*) scratch, sizeof (PROFILE_STEP), &bw);
        }
        ff_Read(&globalstate.Profile, (BYTE*) & current, sizeof (PROFILE_STEP), &bw);
        current.Duration -= globalstate.StepTimeRemaining;
        current.EndTemperature = (globalstate.StepTemperature);
        ff_Append(&newProfile, (BYTE*) & current, sizeof (PROFILE_STEP), &bw);
        ff_UpdateLength(&newProfile);
        ff_copy("tempprofile", 0, globalstate.Profile.FileName, 0, 0, 1);
        ff_Delete("tempprofile");
        ff_UpdateLength(&globalstate.trendFile);
        //RLE_close(&globalstate.trend_RLE_State);
    }

    globalstate.ManualSetPoint = 0;
    globalstate.ActiveProfileName[0] = 0;
    globalstate.ProfileStartTime = 0;
    globalstate.StepCount = 0;
    globalstate.StepIdx = 0;

    WriteRecoveryRecord(&globalstate);

    return 1;
}

int SetManualMode(float Setpoint, char *msg) {
    if (globalstate.SystemMode == SYSTEMMODE_Profile) {
        sprintf(msg, "Error: Profile is running");
        return -1;
    }

    DISABLE_INTERRUPTS;
    if (globalstate.SystemMode == SYSTEMMODE_IDLE) {
        PID_Initialize(&globalstate.ProcessPID, 0);
        PID_Initialize(&globalstate.TargetPID, 0);
        globalstate.SystemMode = SYSTEMMODE_Manual;
    }
    globalstate.ManualSetPoint = Setpoint;
    ENABLE_INTERRUPTS;

    WriteRecoveryRecord(&globalstate);

    return 1;
}

int TruncateProfile(unsigned char *NewProfileData, int len, char *msg) {
    char scratch[sizeof (PROFILE_STEP)];
    PROFILE_STEP current;
    int bw;
    ff_File *existing = &globalstate.Profile;

    ff_Seek(existing, 0);

    if (globalstate.SystemMode != SYSTEMMODE_Profile) {
        sprintf(msg, "Error: Profile is not running");
        return -1;
    }

    suspendTempCon = 1;

    ff_File newProfile;
    ff_Delete("tempprofile");
    ff_OpenByFileName(&newProfile, "tempprofile", 1);
    int idx;
    for (idx = 0; idx < globalstate.StepIdx; idx++) {
        ff_Read(existing, (BYTE*) scratch, sizeof (PROFILE_STEP), &bw);
        ff_Append(&newProfile, (BYTE*) scratch, sizeof (PROFILE_STEP), &bw);
    }
    ff_Read(&globalstate.Profile, (BYTE*) & current, sizeof (PROFILE_STEP), &bw);
    current.Duration -= globalstate.StepTimeRemaining;
    current.EndTemperature = globalstate.StepTemperature;
    ff_Append(&newProfile, (BYTE*) & current, sizeof (PROFILE_STEP), &bw);

    //Copy the new step data into the new file.
    ff_Append(&newProfile, NewProfileData, len, &bw);


    ff_UpdateLength(&newProfile);
    ff_copy("tempprofile", 0, globalstate.Profile.FileName, 0, 0, 1);
    ff_Delete("tempprofile");

    ff_OpenByFileName(&globalstate.Profile, globalstate.Profile.FileName, 0);

    suspendTempCon = 0;

    return 1;
}

void WriteSampleToDisk(TREND_RECORD *thisRecord) {
    int bytesWritten;
    unsigned long trendFileTargetPosition = 0;
    unsigned long Sample;
    int ProccessTemp, TargetTemp, OutputTemp;

    ProccessTemp = (int) ((thisRecord->ProcessTemperature + 25) * 6.819979813);
    TargetTemp = (int) ((thisRecord->TargetTemperature + 25) * 6.819979813);
    OutputTemp = (int) ((thisRecord->Output + 100));

    if (ProccessTemp < 0) ProccessTemp = 0;
    if (TargetTemp < 0) TargetTemp = 0;
    if (OutputTemp < 0) OutputTemp = 0;
    if (ProccessTemp > 1023) ProccessTemp = 1023;
    if (TargetTemp > 1023) TargetTemp = 1023;
    if (OutputTemp > 1023) OutputTemp = 1023;

    Sample = ProccessTemp;
    Sample <<= 10;
    Sample += TargetTemp;
    Sample <<= 10;
    Sample += OutputTemp;
    Sample <<= 1;
    if (thisRecord->Relay & 0b01) Sample += 1;
    Sample <<= 1;
    if (thisRecord->Relay & 0b10) Sample += 1;
    trendFileTargetPosition = thisRecord->time * TREND_RECORD_SIZE;
    if (globalstate.trendFile.Position != trendFileTargetPosition) ff_Seek(&globalstate.trendFile, trendFileTargetPosition);
    ff_Append(&globalstate.trendFile, (BYTE *) & Sample, TREND_RECORD_SIZE, &bytesWritten);
    Log("Record, %ul,(%i)%f2, (%i)%f2, (%i)%f2, %xb",
            thisRecord->time,
            ProccessTemp, thisRecord->ProcessTemperature,
            TargetTemp, thisRecord->TargetTemperature,
            OutputTemp, thisRecord->Output, thisRecord->Relay);
    Log(", %xl, %xl, %xl\r\n",
            globalstate.trendFile.Position,
            globalstate.trendFile.CurrentSector,
            globalstate.trendFile.SectorOffset);
}

void FixedOffTimePWM(MACHINE_STATE *state) {
    if (state->Output > 0) {
        if (state->CoolRelay) {
            //Oops - The controller is calling for heat but we are currently cooling!
            if (state->SystemTime > state->CoolWhenCanTurnOff) {
                state->HeatRelay = 0;
                state->CoolRelay = 0;
                state->CoolWhenCanTurnOn = state->SystemTime + state->equipmentConfig.CoolMinTimeOff;
                if (debugTemperatureController) Log(",COOLTURNEDOFF");
            } else {
                if (debugTemperatureController) Log(",WAITCOOLOFF");
            }
        } else if (state->HeatRelay) { //Currently Heating...
            float TimeOn = state->SystemTime - state->TimeTurnedOn;
            float Duty = (TimeOn / (TimeOn + state->equipmentConfig.HeatMinTimeOff));
            if (Duty > state->Output) {
                if (state->SystemTime > state->HeatWhenCanTurnOff) {
                    state->HeatRelay = 0;
                    state->CoolRelay = 0;
                    state->HeatWhenCanTurnOn = state->SystemTime + state->equipmentConfig.HeatMinTimeOff;
                    if (debugTemperatureController) Log(",HEATTURNEDOFF");
                } else {
                    if (debugTemperatureController) Log(",WAITHEATOFF");
                }
            } else {
                if (debugTemperatureController) Log(",HEATING,%f2", Duty);
            }
        } else {
            float EstOnTime = state->equipmentConfig.HeatMinTimeOn;
            if (state->Output < 0.99) {
                EstOnTime = (state->Output * state->equipmentConfig.HeatMinTimeOff) / (1 - state->Output);
            }
            if (EstOnTime >= state->equipmentConfig.HeatMinTimeOn) {
                if (state->SystemTime > state->HeatWhenCanTurnOn) {
                    state->TimeTurnedOn = state->SystemTime;
                    state->HeatRelay = 1;
                    state->CoolRelay = 0;
                    state->HeatWhenCanTurnOff = state->SystemTime + state->equipmentConfig.HeatMinTimeOn;
                    if (debugTemperatureController) Log(",HEATTURNEDON");
                } else {
                    if (debugTemperatureController) Log(",WAITHEATON");
                }
            } else {
                if (debugTemperatureController) Log(",HEAT_TOOSHORT,%f1", EstOnTime);
            }
        }
    } else if (state->Output < 0) {
        if (state->HeatRelay) {
            //Oops - The controller is calling for COOL but we are currently Heating!
            if (state->SystemTime > state->HeatWhenCanTurnOff) {
                state->HeatRelay = 0;
                state->CoolRelay = 0;
                state->HeatWhenCanTurnOn = state->SystemTime + state->equipmentConfig.HeatMinTimeOff;
                if (debugTemperatureController) Log(",HEATTURNEDOFF");
            } else {
                if (debugTemperatureController) Log(",WAITHEATOFF");
            }
        } else if (state->CoolRelay) { //Currently Cooling...
            float TotalTimeOn = state->SystemTime - state->TimeTurnedOn;
            float Duty = (TotalTimeOn / (TotalTimeOn + state->equipmentConfig.CoolMinTimeOff));
            if (Duty > -state->Output) {
                if (state->SystemTime > state->CoolWhenCanTurnOff) {
                    state->HeatRelay = 0;
                    state->CoolRelay = 0;
                    state->CoolWhenCanTurnOn = state->SystemTime + state->equipmentConfig.CoolMinTimeOff;
                    if (debugTemperatureController) Log(",COOLTURNEDOFF");
                } else {
                    if (debugTemperatureController) Log(",COOLWAITOFF");
                }
            } else {
                if (debugTemperatureController) Log(",COOLING,%f2:", Duty);
            }
        } else {
            float EstOnTime = state->equipmentConfig.CoolMinTimeOn * 2;
            if (state->Output > -0.99) {
                EstOnTime = ((-state->Output) * state->equipmentConfig.CoolMinTimeOff) / (1 + state->Output);
            }
            if (EstOnTime >= state->equipmentConfig.CoolMinTimeOn) {
                if (state->SystemTime > state->CoolWhenCanTurnOn) {
                    state->TimeTurnedOn = state->SystemTime;
                    state->CoolRelay = 1;
                    state->HeatRelay = 0;
                    state->CoolWhenCanTurnOff = state->SystemTime + state->equipmentConfig.CoolMinTimeOn;
                    if (debugTemperatureController) Log(",COOLTURNEDON");
                } else {
                    if (debugTemperatureController) Log(",COOLWAITON");
                }
            } else {
                if (debugTemperatureController) Log(",COOL_TOOSHORT,%f1", EstOnTime);
            }
        }
    } else {
        if (state->CoolRelay) {

            if (state->SystemTime > state->CoolWhenCanTurnOff) {
                state->HeatRelay = 0;
                state->CoolRelay = 0;
                state->CoolWhenCanTurnOn = state->SystemTime + state->equipmentConfig.CoolMinTimeOff;
                if (debugTemperatureController) Log(",COOLTURNEDOFF");
            } else {
                if (debugTemperatureController) Log(",WAITCOOLOFF");
            }
        }
        if (state->HeatRelay) {

            if (state->SystemTime > state->HeatWhenCanTurnOff) {
                state->HeatRelay = 0;
                state->CoolRelay = 0;
                state->HeatWhenCanTurnOn = state->SystemTime + state->equipmentConfig.HeatMinTimeOff;
                if (debugTemperatureController) Log(",HEATTURNEDOFF");
            } else {

                if (debugTemperatureController) Log(",WAITHEATOFF");
            }
        }
    }
}

void TemperatureController_ProcessLoop() {
    TREND_RECORD sample;
    char scratch[256];
    static float smoothedProcessTemperature = 0;
    static float smoothedTargetTemperature = 0;
    static int spikeDetectorIdx = 0;
    static float spikeDetectorTarget[5];
    static float spikeDetectorProcess[5];
    static int firstRun = 1;
    static int OnOffMode = -1;
    static unsigned long lastTimeSeconds = 0;
    static unsigned long lastTimeMinutes = 0;
    if (isTemperatureController_Initialized == 0) return;
    float CommandedTemperature = 0;
    static float CutInTemperature, ProcessSetPoint;
    int x;
    float accumP, accumT;
    float TargetMidline, ProcessMidline;
    BYTE relay = 0;

    if (globalstate.SystemTime == lastTimeSeconds) return; //Nothing to do...
    if (suspendTempCon) return;

    lastTimeSeconds = globalstate.SystemTime;

    ReadTemperature(&globalstate);

    if (firstRun) {
        for (x = 0; x < 5; x++) {
            spikeDetectorProcess[x] = globalstate.ProcessTemperature;
            spikeDetectorTarget[x] = globalstate.TargetTemperature;
        }
        firstRun = 0;
        spikeDetectorIdx = 0;
        smoothedProcessTemperature = globalstate.ProcessTemperature;
        smoothedTargetTemperature = globalstate.TargetTemperature;
    } else {
        spikeDetectorProcess[spikeDetectorIdx] = globalstate.ProcessTemperature;
        spikeDetectorTarget[spikeDetectorIdx] = globalstate.TargetTemperature;
        spikeDetectorIdx++;
        if (spikeDetectorIdx >= 5) spikeDetectorIdx = 0;
    }

    accumP = 0;
    accumT = 0;
    for (x = 0; x < 5; x++) {
        accumP += spikeDetectorProcess[x];
        accumT += spikeDetectorTarget[x];
    }
    accumP /= 5;
    accumT /= 5;
    ProcessMidline = accumP;
    TargetMidline = accumT;

    int diffT = TargetMidline - globalstate.TargetTemperature;
    int diffP = ProcessMidline - globalstate.ProcessTemperature;

    if (diffT < 2 && diffT>-2 && diffP < 2 && diffP>-2) {
        smoothedProcessTemperature = (0.5 * smoothedProcessTemperature)+(0.5 * globalstate.ProcessTemperature);
        smoothedTargetTemperature = (0.5 * smoothedTargetTemperature)+(0.5 * globalstate.TargetTemperature);
    }

    if (debugTemperatureController) Log("TICK, %ul, %f3, %f3", globalstate.SystemTime, smoothedTargetTemperature, smoothedProcessTemperature);

    if (smoothedTargetTemperature < -250 || smoothedProcessTemperature <-250 || smoothedProcessTemperature > 1250 || smoothedTargetTemperature > 1250) {
        if (debugTemperatureController) Log(", INVALID_TEMP\r\n");
        return;
    }

    if (globalstate.SystemMode == SYSTEMMODE_IDLE || globalstate.SystemMode == SYSTEMMODE_ProfileEnded) {
        CommandedTemperature = -32767;
        globalstate.Output = 0;
        globalstate.HeatRelay = 0;
        globalstate.CoolRelay = 0;
        if (debugTemperatureController) Log(", Idle");
        goto OnExit;
    } else if (globalstate.SystemMode == SYSTEMMODE_Profile) {
        unsigned long elapsedTime = globalstate.SystemTime - globalstate.ProfileStartTime;
        globalstate.totalElapsedProfileTime = elapsedTime;
        unsigned long thisTimeMinutes = elapsedTime / 60;

        //Go to the start of the profile step data...
        ff_Seek(&globalstate.Profile, 64);
        PROFILE_STEP step;
        char ProfileComplete = 1;
        int stepIdx = 0;
        int bw;
        ff_Seek(&(globalstate.Profile), 0);
        while ((globalstate.Profile.Length - globalstate.Profile.Position) >= sizeof (PROFILE_STEP)) {
            ff_Read(&globalstate.Profile, (BYTE *) & step, sizeof (PROFILE_STEP), &bw);
            if (elapsedTime < step.Duration) {
                globalstate.StepIdx = stepIdx;
                globalstate.StepTimeRemaining = step.Duration - elapsedTime;
                globalstate.StepTemperature = step.StartTemperature + ((step.EndTemperature - step.StartTemperature) * ((float) elapsedTime / (float) step.Duration));
                CommandedTemperature = globalstate.StepTemperature;
                ProfileComplete = 0;
                break;
            } else {
                elapsedTime -= step.Duration;
            }
            stepIdx++;
        }

        //Log Temperature HERE
        if (lastTimeMinutes != thisTimeMinutes) {
            lastTimeMinutes = thisTimeMinutes;
            sample.ProcessTemperature = smoothedProcessTemperature;
            sample.TargetTemperature = smoothedTargetTemperature;

            if (globalstate.equipmentConfig.RegulationMode == REGULATIONMODE_ComplexThreshold) {
                sample.Output = globalstate.TargetPID.Output;
            } else {
                sample.Output = globalstate.Output * 100;
            }

            sample.time = thisTimeMinutes;
            relay = 0;
            if (globalstate.HeatRelay) relay |= 0b01;
            if (globalstate.CoolRelay) relay |= 0b10;
            sample.Relay = relay;

            //Commit to disk
            WriteSampleToDisk(&sample);
        }

        if (debugTemperatureController) Log(", PRFL, %ul, %ui, %ul, %f2", elapsedTime, globalstate.StepIdx, globalstate.StepTimeRemaining, globalstate.StepTemperature);

        if (ProfileComplete) {
            TerminateProfile(scratch);
            CommandedTemperature = -32767;
            globalstate.Output = 0;
            goto OnExit;
        }
    } else {
        if (debugTemperatureController) Log(", Man,");
        CommandedTemperature = globalstate.ManualSetPoint;
        if (debugTemperatureController) Log(", %i", CommandedTemperature);
    }

    switch (globalstate.equipmentConfig.RegulationMode) {
        case REGULATIONMODE_ComplexPID:

            globalstate.TargetPID.Setpoint = CommandedTemperature;
            globalstate.TargetPID.Input = smoothedTargetTemperature;
            PID_Compute(&globalstate.TargetPID);

            globalstate.ProcessPID.Setpoint = globalstate.TargetPID.Output;
            globalstate.ProcessPID.Input = smoothedProcessTemperature;
            PID_Compute(&globalstate.ProcessPID);

            globalstate.Output = (char) globalstate.ProcessPID.Output;
            if (debugTemperatureController) Log(",Cpid, %f2, %f2, %f2, %f2, %f2, %f2, %f2, %f2",
                    globalstate.TargetPID.error,
                    globalstate.TargetPID.ITerm,
                    globalstate.TargetPID.DTerm,
                    globalstate.TargetPID.Output,
                    globalstate.ProcessPID.error,
                    globalstate.ProcessPID.ITerm,
                    globalstate.ProcessPID.DTerm,
                    globalstate.ProcessPID.Output);
            break;
        case REGULATIONMODE_SimplePID:
            globalstate.ProcessPID.Setpoint = CommandedTemperature;
            globalstate.ProcessPID.Input = smoothedTargetTemperature;
            PID_Compute(&globalstate.ProcessPID);

            globalstate.Output = (char) globalstate.ProcessPID.Output;

            if (debugTemperatureController) Log(", Spid, %f2, %f2, %f2, %f2",
                    globalstate.ProcessPID.error,
                    globalstate.ProcessPID.ITerm,
                    globalstate.ProcessPID.DTerm,
                    globalstate.ProcessPID.Output);
            break;
        case REGULATIONMODE_SimpleThreashold:
            if (debugTemperatureController) Log(", SimpT, NOT IMPLEMENTED");
            break;
        case REGULATIONMODE_ComplexThreshold:
            globalstate.TargetPID.Setpoint = CommandedTemperature;
            globalstate.TargetPID.Input = smoothedTargetTemperature;

            PID_SetOutputLimits(&globalstate.TargetPID,
                    globalstate.equipmentConfig.TargetOutput_Min - smoothedTargetTemperature,
                    globalstate.equipmentConfig.TargetOutput_Max - smoothedTargetTemperature);

            PID_Compute(&globalstate.TargetPID);

            if (debugTemperatureController) Log(", CplxT, %f2, %f2, %f2, %f2",
                    globalstate.TargetPID.error,
                    globalstate.TargetPID.ITerm,
                    globalstate.TargetPID.DTerm,
                    globalstate.TargetPID.Output);

            ProcessSetPoint = smoothedTargetTemperature + globalstate.TargetPID.Output;

            if (globalstate.TargetPID.Output < (globalstate.equipmentConfig.coolDifferential * 0.1)) {
                OnOffMode = -1;
            } else if (globalstate.TargetPID.Output > (globalstate.equipmentConfig.heatDifferential * 0.1)) {
                OnOffMode = 1;
            }

            if (OnOffMode < 0) {
                CutInTemperature = ProcessSetPoint + (globalstate.equipmentConfig.coolDifferential * 0.5);
            } else {
                CutInTemperature = ProcessSetPoint - (globalstate.equipmentConfig.heatDifferential * 0.5);
            }

            if (CutInTemperature > globalstate.equipmentConfig.TargetOutput_Max) {
                CutInTemperature = globalstate.equipmentConfig.TargetOutput_Max;
            } else if (CutInTemperature < globalstate.equipmentConfig.TargetOutput_Min) {
                CutInTemperature = globalstate.equipmentConfig.TargetOutput_Min;
            }

            if (debugTemperatureController) Log(", %f2", CutInTemperature);

            if (OnOffMode < 0) { //Cooling Mode
                if (globalstate.Output < 0) {
                    //Cooling is ON so only turn off after we've crossed the setpoint+differential.
                    if (smoothedProcessTemperature < (CutInTemperature - globalstate.equipmentConfig.coolDifferential)) globalstate.Output = 0;
                } else {
                    globalstate.Output = 0;
                    //Cooling is OFF so wait until the temperature rises above the setpoint to turn on...
                    if (smoothedProcessTemperature > CutInTemperature) globalstate.Output = -1;
                }
            } else { //Heating Mode

                if (globalstate.Output > 0) {
                    //Heating is ON so only turn off after we've crossed the setpoint+differential.
                    if (smoothedProcessTemperature > (CutInTemperature + globalstate.equipmentConfig.heatDifferential)) globalstate.Output = 0;
                } else {
                    globalstate.Output = 0;
                    //Heating is OFF so wait until the temperature falls below the setpoint...
                    if (smoothedProcessTemperature < CutInTemperature) globalstate.Output = 1;
                }
            }

            if (globalstate.Output > 0) {
                if (debugTemperatureController) Log(", HEAT");
                globalstate.Output = 1;
            } else if (globalstate.Output < 0) {
                if (debugTemperatureController) Log(", COOL");
                globalstate.Output = -1;
            } else {
                if (debugTemperatureController) Log(", OFF");
                globalstate.Output = 0;
            }
            break;
    }

    //Calculate the PWM
    FixedOffTimePWM(&globalstate);

OnExit:
    TemperatureControllerIsAlive = 1;
    SET_HEAT(globalstate.HeatRelay);
    SET_COOL(globalstate.CoolRelay);
    if (debugTemperatureController) Log("\n");

}

int TemperatureController_Initialize() {
    if (isTemperatureController_Initialized == 1) return 1;
    if (RestoreRecoveryRecord() == 0) {
        //There was no recovery record or it was corrupted - default settings...
        InitializeRecoveryRecord();
        RestoreRecoveryRecord();
    }

    //Set the system time...
    globalstate.SystemTime = RTC_GetTime();
    isTemperatureController_Initialized = 1;

    return 1;
}
