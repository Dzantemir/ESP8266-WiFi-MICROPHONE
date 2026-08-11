' ============================================================================
'  audio_codec.inc — Audio codec (IMA ADPCM, ring buffer, BWSOLA PLC)
' ============================================================================
#IF NOT %DEF(%AUDIO_CODEC_INC)
%AUDIO_CODEC_INC = 1

' DecodeImaAdpcm - Decode IMA ADPCM data to 16-bit PCM
'
' Predictor update uses 32-bit arithmetic + clamp, NOT CINT(diff) - CINT()
' truncates to 16-bit BEFORE the add and overflows at step_index >= 87
' (max delta 55863..61438 > 32767), corrupting the predictor on loud transients.
' Mirrors ESP encoder's clamp_int16(predictor + delta).
SUB DecodeImaAdpcm(BYVAL pSrc AS BYTE PTR, BYVAL srcLen AS LONG, _
                   BYVAL pDst AS INTEGER PTR, predictor AS INTEGER, stepIndex AS LONG)
    LOCAL i AS LONG
    LOCAL b AS BYTE
    LOCAL CODE AS LONG
    LOCAL stp AS LONG
    LOCAL diff AS LONG
    LOCAL tmpPred AS LONG    ' 32-bit scratch for predictor + diff before clamp

    FOR i = 1 TO srcLen
        b = @pSrc
        INCR pSrc

        ' High nibble first
        CODE = (b AND &HF0) \ &H10
        GOSUB DoNibble
        @pDst = predictor
        INCR pDst

        ' Low nibble
        CODE = b AND &H0F
        GOSUB DoNibble
        @pDst = predictor
        INCR pDst
    NEXT i
    EXIT SUB

DoNibble:
    IF stepIndex < 0 THEN stepIndex = 0
    IF stepIndex > 88 THEN stepIndex = 88
    stp = g_StepTable(stepIndex)
    diff = stp \ 8
    IF (CODE AND 4) THEN diff = diff + stp
    IF (CODE AND 2) THEN diff = diff + stp \ 2
    IF (CODE AND 1) THEN diff = diff + stp \ 4
    IF (CODE AND 8) THEN diff = -diff

    ' 32-bit add + clamp - mirrors the ESP encoder's clamp_int16(predictor + delta).
    ' Do NOT use CINT(diff): it truncates to 16-bit BEFORE the add and overflows
    ' at step_index >= 87 (max delta 55863..61438 > 32767), corrupting the
    ' predictor on loud transients.
    tmpPred = CLNG(predictor) + diff
    IF tmpPred < -32768 THEN tmpPred = -32768
    IF tmpPred > 32767 THEN tmpPred = 32767
    predictor = tmpPred

    stepIndex = stepIndex + g_IndexTable(CODE AND 7)
    IF stepIndex < 0 THEN stepIndex = 0
    IF stepIndex > 88 THEN stepIndex = 88
    RETURN
END SUB

' HzToRateEnum - Convert sample rate in Hz to EASSP enum (0..6).
' Returns 2 (16kHz) as fallback for unknown rates.
FUNCTION HzToRateEnum(BYVAL Hz AS DWORD) AS LONG
    SELECT CASE Hz
        CASE 8000:  FUNCTION = 0
        CASE 11025: FUNCTION = 1
        CASE 16000: FUNCTION = 2
        CASE 22050: FUNCTION = 3
        CASE 32000: FUNCTION = 4
        CASE 44100: FUNCTION = 5
        CASE 48000: FUNCTION = 6
        CASE ELSE:  FUNCTION = 2   ' fallback 16kHz
    END SELECT
END FUNCTION

' RateEnumToHz - Convert EASSP sample-rate enum (0..6) to Hz.
' Returns 16000 as fallback for out-of-range enums.
FUNCTION RateEnumToHz(BYVAL e AS LONG) AS DWORD
    SELECT CASE e
        CASE 0: FUNCTION = 8000
        CASE 1: FUNCTION = 11025
        CASE 2: FUNCTION = 16000
        CASE 3: FUNCTION = 22050
        CASE 4: FUNCTION = 32000
        CASE 5: FUNCTION = 44100
        CASE 6: FUNCTION = 48000
        CASE ELSE: FUNCTION = 16000
    END SELECT
END FUNCTION

' EspActualRate - Computes ACTUAL ESP8266 I2S rate for given nominal Hz / bits.
' Mirrors i2s_set_rate() from firmware/i2s.c: 160 MHz / (bits*2ch) / (bck_div * mclk_div).
'
' WHY: ESP cannot produce exactly 44100 Hz (real = 43860 Hz, -0.545%).
' Opening WaveOut at nominal 44100 underruns -> clicks. Open at actual ESP
' rate -> drift=0; Windows audio engine does the final resample.
FUNCTION EspActualRate(BYVAL nominalHz AS DWORD, BYVAL i2sBits AS LONG) AS DWORD
    LOCAL scaled AS DOUBLE
    LOCAL i AS LONG, j AS LONG
    LOCAL bestI AS LONG, bestJ AS LONG
    LOCAL bestDelta AS DOUBLE, d AS DOUBLE, actual AS DOUBLE

    ' 160 МГц / (bits × 2ch)
    IF i2sBits = 24 THEN
        scaled = 160000000.0# / 48.0#     ' 3 333 333.33
    ELSE
        scaled = 160000000.0# / 32.0#     ' 5 000 000
    END IF

    ' Перебор всех пар делителей 1..63 (как в прошивке)
    bestI = 1
    bestJ = 1
    bestDelta = scaled                  ' больше любого возможного delta
    FOR i = 1 TO 63
        FOR j = i TO 63                 ' j >= i: симметрично, отсекаем дубли
            actual = scaled / (CDBL(i) * CDBL(j))
            d = ABS(actual - nominalHz)
            IF d < bestDelta THEN
                bestDelta = d
                bestI = i
                bestJ = j
                IF d = 0! THEN EXIT FOR ' точное попадание
            END IF
        NEXT j
        IF bestDelta = 0! THEN EXIT FOR
    NEXT i

    actual = scaled / (CDBL(bestI) * CDBL(bestJ))
    FUNCTION = CLNG(actual)
END FUNCTION

' CodecName - Return human-readable codec name from codec ID.
FUNCTION CodecName(BYVAL codecId AS LONG) AS STRING
    SELECT CASE codecId
        CASE %CODEC_ID:     FUNCTION = "ADPCM"
        CASE %CODEC_ID_PCM: FUNCTION = "PCM"
        CASE ELSE:          FUNCTION = "Unknown(" & TRIM$(STR$(codecId)) & ")"
    END SELECT
END FUNCTION

' RingWrite - copy nBytes from pSrc into ring. On overflow: drop OLDEST bytes.
' ringW/ringR/ringUsed are passed BYREF and updated in place.
FUNCTION RingWrite(BYVAL pRing AS DWORD, BYVAL ringCap AS LONG, _
                   BYREF ringW AS LONG, BYREF ringR AS LONG, BYREF ringUsed AS LONG, _
                   BYVAL pSrc AS DWORD, BYVAL nBytes AS LONG) AS LONG
    LOCAL dropped AS LONG, chunk AS LONG, bytesLeft AS LONG
    LOCAL pD AS DWORD, pS AS DWORD

    IF nBytes <= 0 OR pRing = 0 OR pSrc = 0 THEN
        FUNCTION = 0
        EXIT FUNCTION
    END IF
    ' Frame larger than whole ring: keep only the last ringCap bytes
    IF nBytes > ringCap THEN
        pSrc   = pSrc + (nBytes - ringCap)
        nBytes = ringCap
    END IF

    dropped = 0
    ' Make room by dropping oldest bytes (advance ringR) until fit
    DO WHILE ringUsed + nBytes > ringCap
        chunk = ringUsed + nBytes - ringCap
        IF chunk > ringUsed THEN chunk = ringUsed
        IF chunk <= 0 THEN EXIT DO
        ringR = (ringR + chunk) MOD ringCap
        ringUsed = ringUsed - chunk
        dropped = 1
    LOOP

    ' Copy in chunks (may wrap around ringW -> 0)
    bytesLeft = nBytes
    pS = pSrc
    DO WHILE bytesLeft > 0
        chunk = ringCap - ringW
        IF chunk > bytesLeft THEN chunk = bytesLeft
        pD = pRing + ringW
        CopyMemory pD, pS, chunk
        ringW = (ringW + chunk) MOD ringCap
        pS = pS + chunk
        bytesLeft = bytesLeft - chunk
    LOOP
    ringUsed = ringUsed + nBytes
    FUNCTION = dropped
END FUNCTION

' RingRead - copy nBytes from ring to pDst if available.
' Returns nBytes on success, 0 if not enough data (or invalid params).
' ringR/ringUsed are passed BYREF and updated in place. ringW is read-only
' here but kept in signature for symmetry with RingWrite.
FUNCTION RingRead(BYVAL pRing AS DWORD, BYVAL ringCap AS LONG, _
                  BYREF ringW AS LONG, BYREF ringR AS LONG, BYREF ringUsed AS LONG, _
                  BYVAL pDst AS DWORD, BYVAL nBytes AS LONG) AS LONG
    LOCAL bytesLeft AS LONG, chunk AS LONG
    LOCAL pS AS DWORD, pD AS DWORD

    IF nBytes <= 0 OR ringUsed < nBytes OR pRing = 0 OR pDst = 0 THEN
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    bytesLeft = nBytes
    pD = pDst
    DO WHILE bytesLeft > 0
        chunk = ringCap - ringR
        IF chunk > bytesLeft THEN chunk = bytesLeft
        pS = pRing + ringR
        CopyMemory pD, pS, chunk
        ringR = (ringR + chunk) MOD ringCap
        pD = pD + chunk
        bytesLeft = bytesLeft - chunk
    LOOP
    ringUsed = ringUsed - nBytes
    FUNCTION = nBytes
END FUNCTION

' RingFillZero - write nBytes of silence (zeros) to pDst. Used for underrun
' injection in FeedWaveOut (one silence frame to avoid hard WaveOut click).
SUB RingFillZero(BYVAL pDst AS DWORD, BYVAL nBytes AS LONG)
    IF pDst = 0 OR nBytes <= 0 THEN EXIT SUB
    FillMemory pDst, nBytes, 0
END SUB

' BWSOLA_IsVoiced - Detect voiced speech via normalized autocorrelation.
' Computes ACF(lag) for lag in [PITCH_MIN*nCh .. PITCH_MAX*nCh] step nCh.
FUNCTION BWSOLA_IsVoiced(BYVAL pPcm AS INTEGER PTR, BYVAL nSmp AS LONG, _
                         BYVAL nCh AS LONG, BYVAL smpRate AS DWORD) AS LONG
    LOCAL i AS LONG, lag AS LONG
    LOCAL acf0 AS QUAD, acfLag AS QUAD
    LOCAL maxNacf AS DOUBLE, nacf AS DOUBLE
    LOCAL s0 AS LONG, sLag AS LONG
    LOCAL pitchMin AS LONG, pitchMax AS LONG

    IF nCh < 1 THEN nCh = 1
    IF nCh > 2 THEN nCh = 2
    pitchMin = (%BWSOLA_PITCH_MIN * smpRate \ 48000) * nCh
    pitchMax = (%BWSOLA_PITCH_MAX * smpRate \ 48000) * nCh
    ' Need at least pitchMin+1 samples to compute ACF at the minimum lag.
    ' Shorter frames (e.g. 5ms @ 48kHz = 240 samples) can still detect voices
    ' with pitch >= smpRate/nSmp; the inner loop EXIT FORs when lag >= nSmp.
    IF nSmp < pitchMin + 1 THEN
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    ' ACF(0) = sum of x[i]^2. QUAD accumulator avoids LONG overflow.
    acf0 = 0
    FOR i = 0 TO nSmp - 1
        s0 = @pPcm[i]
        acf0 = acf0 + CQUD(s0) * CQUD(s0)
    NEXT i
    IF acf0 = 0 THEN
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    ' Find max normalized ACF in [pitchMin..pitchMax], step nCh (same-channel)
    maxNacf = 0
    FOR lag = pitchMin TO pitchMax STEP nCh
        IF lag >= nSmp THEN EXIT FOR
        acfLag = 0
        FOR i = 0 TO nSmp - lag - 1
            s0 = @pPcm[i]
            sLag = @pPcm[i + lag]
            acfLag = acfLag + CQUD(s0) * CQUD(sLag)
        NEXT i
        nacf = CDBL(acfLag) / CDBL(acf0)
        IF nacf > maxNacf THEN maxNacf = nacf
    NEXT lag

    IF maxNacf > (CDBL(%BWSOLA_VOICED_THRESH) / 100.0#) THEN
        FUNCTION = 1
    ELSE
        FUNCTION = 0
    END IF
END FUNCTION

' BWSOLA_FindPitch - Find pitch period via normalized autocorrelation.
' Returns best lag (pitch period in FLAT samples) in [PITCH_MIN*nCh..PITCH_MAX*nCh],
' or 0 if unvoiced (no ACF peak above threshold).
FUNCTION BWSOLA_FindPitch(BYVAL pPcm AS INTEGER PTR, BYVAL nSmp AS LONG, _
                          BYVAL nCh AS LONG, BYVAL smpRate AS DWORD) AS LONG
    LOCAL i AS LONG, lag AS LONG
    LOCAL acf0 AS QUAD, acfLag AS QUAD
    LOCAL maxNacf AS DOUBLE, nacf AS DOUBLE
    LOCAL bestLag AS LONG
    LOCAL s0 AS LONG, sLag AS LONG
    LOCAL pitchMin AS LONG, pitchMax AS LONG

    IF nCh < 1 THEN nCh = 1
    IF nCh > 2 THEN nCh = 2
    pitchMin = (%BWSOLA_PITCH_MIN * smpRate \ 48000) * nCh
    pitchMax = (%BWSOLA_PITCH_MAX * smpRate \ 48000) * nCh
    ' Need at least pitchMin+1 samples to compute ACF at the minimum lag.
    ' Shorter frames (e.g. 5ms @ 48kHz = 240 samples) can still detect voices
    ' with pitch >= smpRate/nSmp; the inner loop EXIT FORs when lag >= nSmp.
    IF nSmp < pitchMin + 1 THEN
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    acf0 = 0
    FOR i = 0 TO nSmp - 1
        s0 = @pPcm[i]
        acf0 = acf0 + CQUD(s0) * CQUD(s0)
    NEXT i
    IF acf0 = 0 THEN
        FUNCTION = 0
        EXIT FUNCTION
    END IF

    maxNacf = 0
    bestLag = 0
    FOR lag = pitchMin TO pitchMax STEP nCh
        IF lag >= nSmp THEN EXIT FOR
        acfLag = 0
        FOR i = 0 TO nSmp - lag - 1
            s0 = @pPcm[i]
            sLag = @pPcm[i + lag]
            acfLag = acfLag + CQUD(s0) * CQUD(sLag)
        NEXT i
        nacf = CDBL(acfLag) / CDBL(acf0)
        IF nacf > maxNacf THEN
            maxNacf = nacf
            bestLag = lag
        END IF
    NEXT lag

    IF maxNacf > (CDBL(%BWSOLA_VOICED_THRESH) / 100.0#) THEN
        FUNCTION = bestLag
    ELSE
        FUNCTION = 0
    END IF
END FUNCTION

' BWSOLA_Clamp16 - Clamp 32-bit sample to signed 16-bit before store.
' clamp before INTEGER store to prevent overflow distortion.
FUNCTION BWSOLA_Clamp16(BYVAL v AS LONG) AS INTEGER
    IF v < -32768 THEN v = -32768
    IF v >  32767 THEN v =  32767
    FUNCTION = v
END FUNCTION

' BWSOLA_Crossfade16 - Linear OLA crossfade from pA to pB over nFlat samples.
' pOut[i] = pA[i] * (1 - g) + pB[i] * g, where g = i / (nFlat - 1).
' Endpoint-safe: nFlat=1 copies pB[0]. Uses DOUBLE gain, CINT cast for INT16.
' Safe for pA == pOut (self-referential): each pOut[i] is written AFTER
' reading pA[i] = pOut[i], and earlier indices are never re-read.
SUB BWSOLA_Crossfade16(BYVAL pA AS INTEGER PTR, BYVAL pB AS INTEGER PTR, _
                       BYVAL nFlat AS LONG, BYVAL pOut AS INTEGER PTR)
    LOCAL i AS LONG
    LOCAL g AS DOUBLE
    LOCAL sA AS LONG, sB AS LONG

    IF nFlat <= 0 THEN EXIT SUB
    IF nFlat = 1 THEN
        @pOut[0] = @pB[0]
        EXIT SUB
    END IF

    FOR i = 0 TO nFlat - 1
        g = CDBL(i) / CDBL(nFlat - 1)
        sA = @pA[i]
        sB = @pB[i]
        @pOut[i] = BWSOLA_Clamp16(CLNG(CDBL(sA) * (1.0# - g) + CDBL(sB) * g))
    NEXT i
END SUB

' BWSOLA_PLC_Frame - Main bilateral PLC reconstruction (per Yeh et al. 2013).
' Takes L_WSOLA (previous good packet) + R_WSOLA (next good packet), produces
' a concealed frame for the lost packet. Lengths in SAMPLES (flat, interleaved
' if stereo). Returns bytes written (outLen*2) or 0 on failure.
'
' Strategy: BV (both voiced) = bilateral pitch continuation + linear crossfade.
' PV/NV = extend voiced side + amplitude-adjust toward unvoiced energy.
' BU (both unvoiced) = linear crossfade L tail -> R head.
FUNCTION BWSOLA_PLC_Frame(BYVAL pL AS INTEGER PTR, BYVAL lenL AS LONG, _
                          BYVAL pR AS INTEGER PTR, BYVAL lenR AS LONG, _
                          BYVAL pOut AS INTEGER PTR, BYVAL outLen AS LONG, _
                          BYVAL nCh AS LONG, BYVAL smpRate AS DWORD) AS LONG
    LOCAL i AS LONG, k AS LONG
    LOCAL isVL AS LONG, isVR AS LONG
    LOCAL pLflat AS LONG, pRflat AS LONG   ' pitch in flat samples
    LOCAL g AS DOUBLE
    LOCAL sA AS LONG, sB AS LONG
    LOCAL srcIdx AS LONG
    LOCAL energyL AS QUAD, energyR AS QUAD
    LOCAL adjustFact AS DOUBLE

    IF outLen <= 0 OR lenL <= 0 OR lenR <= 0 THEN
        FUNCTION = 0
        EXIT FUNCTION
    END IF
    IF pL = 0 OR pR = 0 OR pOut = 0 THEN
        FUNCTION = 0
        EXIT FUNCTION
    END IF
    IF nCh < 1 THEN nCh = 1
    IF nCh > 2 THEN nCh = 2

    ' Trivial 1-sample case: average L's last sample and R's first sample.
    IF outLen = 1 THEN
        sA = @pL[lenL - 1]
        sB = @pR[0]
        @pOut[0] = BWSOLA_Clamp16(CLNG((CDBL(sA) + CDBL(sB)) / 2.0#))
        FUNCTION = 2
        EXIT FUNCTION
    END IF

    ' Voiced detection + pitch finding on the flat interleaved stream.
    ' BWSOLA_IsVoiced/BWSOLA_FindPitch multiply the per-channel pitch range
    ' by nCh internally, so callers pass the flat buffer + flat sample count.
    isVL = 0
    isVR = 0
    pLflat = 0
    pRflat = 0
    IF lenL >= ((%BWSOLA_PITCH_MIN * smpRate \ 48000) * nCh) + 1 THEN
        isVL = BWSOLA_IsVoiced(pL, lenL, nCh, smpRate)
        IF isVL THEN
            pLflat = BWSOLA_FindPitch(pL, lenL, nCh, smpRate)
            IF pLflat < nCh THEN pLflat = nCh   ' safety
        END IF
    END IF
    IF lenR >= ((%BWSOLA_PITCH_MIN * smpRate \ 48000) * nCh) + 1 THEN
        isVR = BWSOLA_IsVoiced(pR, lenR, nCh, smpRate)
        IF isVR THEN
            pRflat = BWSOLA_FindPitch(pR, lenR, nCh, smpRate)
            IF pRflat < nCh THEN pRflat = nCh
        END IF
    END IF

    IF isVL AND isVR THEN
        ' ---- BV (Both Voiced): bilateral pitch continuation + linear crossfade
        ' L continuation forward: repeat L's last pitch period.
        ' R continuation backward: repeat R's first pitch period (reversed in
        '   time so the END of output = R[pRflat-1], the sample "before" R[0]).
        ' Linear crossfade g = i/(outLen-1): 0 at start (pure L cont), 1 at end
        '   (pure R cont). Smoothly transitions L->R across the gap.
        FOR i = 0 TO outLen - 1
            ' L continuation at position i
            srcIdx = (lenL - pLflat) + (i MOD pLflat)
            IF srcIdx < 0 THEN srcIdx = 0
            IF srcIdx >= lenL THEN srcIdx = lenL - 1
            sA = @pL[srcIdx]
            ' R continuation backward at position i
            k = outLen - 1 - i     ' distance from end (backward in time)
            srcIdx = (pRflat - 1) - (k MOD pRflat)
            IF srcIdx < 0 THEN srcIdx = srcIdx + pRflat
            IF srcIdx >= lenR THEN srcIdx = lenR - 1
            IF srcIdx < 0 THEN srcIdx = 0
            sB = @pR[srcIdx]
            g = CDBL(i) / CDBL(outLen - 1)
            @pOut[i] = BWSOLA_Clamp16(CLNG(CDBL(sA) * (1.0# - g) + CDBL(sB) * g))
        NEXT i

    ELSEIF isVL AND (isVR = 0) THEN
        ' ---- PV (Prev voiced, Next unvoiced) ----
        ' Extend L forward by pitch continuation; amplitude-adjust toward R's
        ' (unvoiced) energy per Eq 11-12: A(n) = 1 - n*(Ev - Euv)/(Ev*(l-1)).
        ' Ev = energy of L (voiced), Euv = energy of R (unvoiced).
        energyL = 0
        FOR i = 0 TO lenL - 1
            sA = @pL[i]
            energyL = energyL + CQUD(sA) * CQUD(sA)
        NEXT i
        energyR = 0
        FOR i = 0 TO lenR - 1
            sB = @pR[i]
            energyR = energyR + CQUD(sB) * CQUD(sB)
        NEXT i
        FOR i = 0 TO outLen - 1
            srcIdx = (lenL - pLflat) + (i MOD pLflat)
            IF srcIdx < 0 THEN srcIdx = 0
            IF srcIdx >= lenL THEN srcIdx = lenL - 1
            sA = @pL[srcIdx]
            IF energyL > 0 THEN
                adjustFact = 1.0# - (CDBL(i) * CDBL(energyL - energyR) / _
                                     (CDBL(energyL) * CDBL(outLen - 1)))
                IF adjustFact < 0.0# THEN adjustFact = 0.0#
                IF adjustFact > 2.0# THEN adjustFact = 2.0#
            ELSE
                adjustFact = 1.0#
            END IF
            @pOut[i] = BWSOLA_Clamp16(CLNG(CDBL(sA) * adjustFact))
        NEXT i

    ELSEIF (isVL = 0) AND isVR THEN
        ' ---- NV (Prev unvoiced, Next voiced) - mirror of PV ----
        ' Extend R backward by pitch continuation; amplitude-adjust toward L's
        ' (unvoiced) energy. A(n) = 1 - n*(Ev - Euv)/(Ev*(l-1)) where now
        ' Ev = energy of R (voiced), Euv = energy of L (unvoiced).
        energyL = 0
        FOR i = 0 TO lenL - 1
            sA = @pL[i]
            energyL = energyL + CQUD(sA) * CQUD(sA)
        NEXT i
        energyR = 0
        FOR i = 0 TO lenR - 1
            sB = @pR[i]
            energyR = energyR + CQUD(sB) * CQUD(sB)
        NEXT i
        FOR i = 0 TO outLen - 1
            k = outLen - 1 - i     ' distance from end (backward in time)
            srcIdx = (pRflat - 1) - (k MOD pRflat)
            IF srcIdx < 0 THEN srcIdx = srcIdx + pRflat
            IF srcIdx >= lenR THEN srcIdx = lenR - 1
            IF srcIdx < 0 THEN srcIdx = 0
            sB = @pR[srcIdx]
            IF energyR > 0 THEN
                adjustFact = 1.0# - (CDBL(outLen - 1 - i) * CDBL(energyR - energyL) / _
                                                     (CDBL(energyR) * CDBL(outLen - 1)))
                IF adjustFact < 0.0# THEN adjustFact = 0.0#
                IF adjustFact > 2.0# THEN adjustFact = 2.0#
            ELSE
                adjustFact = 1.0#
            END IF
            @pOut[i] = BWSOLA_Clamp16(CLNG(CDBL(sB) * adjustFact))
        NEXT i

    ELSE
        ' ---- BU (Both Unvoiced): linear crossfade L tail -> R head ----
        ' Per Section 3.4: treat lost packet as noise, fade-in/fade-out.
        ' Copy L's last outLen samples to pOut, then crossfade with R using
        ' BWSOLA_Crossfade16 (self-referential: pA == pOut is safe).
        ' (theoretical: frame size change before reopen). Use MIN(outLen,lenR)
        ' for the crossfade length; zero-fill the rest toward R's end.
        IF lenL >= outLen THEN
            FOR i = 0 TO outLen - 1
                @pOut[i] = @pL[lenL - outLen + i]
            NEXT i
        ELSE
            ' L shorter than outLen: zero-pad the front, then copy L
            FOR i = 0 TO outLen - lenL - 1
                @pOut[i] = 0
            NEXT i
            FOR i = 0 TO lenL - 1
                @pOut[outLen - lenL + i] = @pL[i]
            NEXT i
        END IF
        LOCAL buSafe AS LONG
        buSafe = outLen
        IF lenR < buSafe THEN buSafe = lenR
        IF buSafe > 0 THEN
            BWSOLA_Crossfade16 pOut, pR, buSafe, pOut
        END IF
        ' Zero-fill any remaining samples beyond buSafe (if lenR < outLen)
        IF buSafe < outLen THEN
            FOR i = buSafe TO outLen - 1
                @pOut[i] = 0
            NEXT i
        END IF
    END IF

    FUNCTION = outLen * 2    ' bytes written (16-bit samples)
END FUNCTION

#ENDIF
