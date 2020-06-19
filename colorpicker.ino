#define DEBUG_COMMS 0
#define null 0
#define CCW_FROM_FACE(f, amt) (((f) - (amt)) + (((f) >= (amt)) ? 0 : 6))

struct RGB
{
  byte r, g, b;
};

struct HSB
{
  byte h, s, b;
};

RGB colorRGB[FACE_COUNT];
HSB colorHSB[FACE_COUNT];

enum eTileRole
{
  kTileRole_Unassigned,
  kTileRole_Palette,
  kTileRole_Param1,
  kTileRole_Param2,
  kTileRole_Param3,
  kTileRole_FaceSelection,
  
  kTileRole_MAX
};
eTileRole tileRole = kTileRole_Unassigned;

byte roleOnFace[FACE_COUNT] = { kTileRole_Unassigned, kTileRole_Unassigned, kTileRole_Unassigned, kTileRole_Unassigned, kTileRole_Unassigned, kTileRole_Unassigned };

enum eFaceSelection
{
  kFaceSelection_0,
  kFaceSelection_1,
  kFaceSelection_2,
  kFaceSelection_3,
  kFaceSelection_4,
  kFaceSelection_5,
  kFaceSelection_All,
  kFaceSelection_HalfNear,
  kFaceSelection_HalfFar,
  kFaceSelection_Alt0,
  kFaceSelection_Alt1,
  kFaceSelection_MAX
};

eFaceSelection faceSelection = kFaceSelection_All;

enum eColorSpace
{
  kColorSpace_RGB,
  kColorSpace_HSB
};
eColorSpace colorSpace = kColorSpace_RGB;

enum eParamDir
{
  kParamDir_Increment,
  kParamDir_Decrement
};
eParamDir paramDir = kParamDir_Increment;

byte paletteFace;
byte ourValue;    // sent from palette to params to display their bitwise value

// =================================================================================================
//
// COMMUNICATIONS
//
// =================================================================================================

#define TOGGLE_COMMAND 1
#define TOGGLE_DATA 0
struct FaceValue
{
  byte value  : 4;
  byte toggle : 1;
  byte ack    : 1;
};

enum NeighborSyncFlag
{
  NeighborSyncFlag_Present    = 1<<0,

  NeighborSyncFlag_Debug      = 1<<7
};

struct FaceState
{
  FaceValue faceValueIn;
  FaceValue faceValueOut;
  byte lastCommandIn;
  byte neighborSyncFlags;

  eTileRole neighborRole;

#if DEBUG_COMMS
  byte ourState;
  byte neighborState;
#endif
};
FaceState faceStates[FACE_COUNT];

#define BOOT_DELAY 500
Timer bootTimer;

enum CommandType
{
  CommandType_None,

  CommandType_Reset,
  CommandType_AssignRole,
  
  CommandType_SetColorSpace,
  
  CommandType_IncParam,
  CommandType_DecParam,
  CommandType_IncMoreParam,
  CommandType_DecMoreParam,

  CommandType_SendingParams,
  
#if DEBUG_COMMS
  CommandType_UpdateState,
#endif
  
  CommandType_MAX
};

struct CommandAndData
{
  CommandType command : 4;
  byte data : 4;
};

#define COMM_QUEUE_SIZE 8
CommandAndData commQueues[FACE_COUNT][COMM_QUEUE_SIZE];

#define COMM_INDEX_ERROR_OVERRUN 0xFF
#define COMM_INDEX_OUT_OF_SYNC   0xFE
#define COMM_DATA_OVERRUN        0xFD
byte commInsertionIndexes[FACE_COUNT];

#define ErrorOnFace(f) (commInsertionIndexes[f] > COMM_QUEUE_SIZE)


#if DEBUG_COMMS
// Timer used to toggle between green & blue
Timer sendNewStateTimer;
#endif

// =================================================================================================

void setup()
{
  bootTimer.set(BOOT_DELAY);
  
//  resetOurState();
  FOREACH_FACE(f)
  {
    resetCommOnFace(f);
  }
}

// =================================================================================================

void loop()
{
  updateCommOnFaces();
  assignRoles();
  if (tileRole == kTileRole_Palette)
  {
    assignNewNeighborRoles();
  }
  processUserInput();

#if DEBUG_COMMS
  if (sendNewStateTimer.isExpired())
  {
    FOREACH_FACE(f)
    {
      byte nextVal = faceStates[f].neighborState == 2 ? 3 : 2;
      faceStates[f].neighborState = nextVal;
      enqueueCommOnFace(f, CommandType_UpdateState, nextVal, true);
    }
    sendNewStateTimer.set(500);
  }
#else // DEBUG_COMMS
#endif // DEBUG_COMMS

  render();
}

// =================================================================================================
//
// COMMUNICATIONS
// Cut-and-paste from sample project
//
// =================================================================================================

void resetCommOnFace(byte f)
{
  // Clear the queue
  commInsertionIndexes[f] = 0;

  FaceState *faceState = &faceStates[f];

  // Put the current output into its reset state.
  // In this case, all zeroes works for us.
  // Assuming the neighbor is also reset, it means our ACK == their TOGGLE.
  // This allows the next pair to be sent immediately.
  // Also, since the toggle bit is set to TOGGLE_DATA, it will toggle into TOGGLE_COMMAND,
  // which is what we need to start sending a new pair.
  faceState->faceValueOut.value = 0;
  faceState->faceValueOut.toggle = TOGGLE_DATA;
  faceState->faceValueOut.ack = TOGGLE_DATA;
  sendValueOnFace(f, faceState->faceValueOut);

  // Mark this neighbor as unassigned so we can assign it again
  roleOnFace[f] = kTileRole_Unassigned;
}

void sendValueOnFace(byte f, FaceValue faceValue)
{
  byte outVal = *((byte*)&faceValue);
  setValueSentOnFace(outVal, f);
}

// Called by the main program when this tile needs to tell something to
// a neighbor tile.
void enqueueCommOnFace(byte f, CommandType commandType, byte data, bool clampToMax)
{
  if (commInsertionIndexes[f] >= COMM_QUEUE_SIZE)
  {
    // Buffer overrun - might need to increase queue size to accommodate
    commInsertionIndexes[f] = COMM_INDEX_ERROR_OVERRUN;
    return;
  }

  if (data & 0xF0)
  {
    if (clampToMax)
    {
      data = 0xF;
    }
    else
    {
      commInsertionIndexes[f] = COMM_DATA_OVERRUN;
    }
  }
  
  byte index = commInsertionIndexes[f];
  commQueues[f][index].command = commandType;
  commQueues[f][index].data = data;
  commInsertionIndexes[f]++;
}

// Called every iteration of loop(), preferably before any main processing
// so that we can act on any new data being sent.
void updateCommOnFaces()
{
  FOREACH_FACE(f)
  {
    // Is the neighbor still there?
    if (isValueReceivedOnFaceExpired(f))
    {
      // Lost the neighbor - no longer in sync
      resetCommOnFace(f);
      faceStates[f].neighborSyncFlags = 0;
      continue;
    }

    // If there is any kind of error on the face then do nothing
    // The error can be reset by removing the neighbor
    if (ErrorOnFace(f))
    {
      continue;
    }

    FaceState *faceState = &faceStates[f];

    faceState->neighborSyncFlags |= NeighborSyncFlag_Present;

    // Read the neighbor's face value it is sending to us
    byte val = getLastValueReceivedOnFace(f);
    faceState->faceValueIn = *((FaceValue*)&val);
    
    //
    // RECEIVE
    //

    // Did the neighbor send a new comm?
    // Recognize this when their TOGGLE bit changed from the last value we got.
    if (faceState->faceValueOut.ack != faceState->faceValueIn.toggle)
    {
      // Got a new comm - process it
      byte value = faceState->faceValueIn.value;
      if (faceState->faceValueIn.toggle == TOGGLE_COMMAND)
      {
        // This is the first part of a comm (COMMAND)
        // Save the command value until we get the data
        faceState->lastCommandIn = value;
      }
      else
      {
        // This is the second part of a comm (DATA)
        // Do application-specific stuff with the comm
        processCommForFace(faceState->lastCommandIn, value, f);
      }

      // Acknowledge that we processed this value so the neighbor can send the next one
      faceState->faceValueOut.ack = faceState->faceValueIn.toggle;
    }
    
    //
    // SEND
    //
    
    // Did the neighbor acknowledge our last comm?
    // Recognize this when their ACK bit equals our current TOGGLE bit.
    if (faceState->faceValueIn.ack == faceState->faceValueOut.toggle)
    {
      // If we just sent the DATA half of the previous comm, check if there 
      // are any more commands to send.
      if (faceState->faceValueOut.toggle == TOGGLE_DATA)
      {
        if (commInsertionIndexes[f] == 0)
        {
          // Nope, no more comms to send - bail and wait
          continue;
        }
      }

      // Send the next value, either COMMAND or DATA depending on the toggle bit

      // Toggle between command and data
      faceState->faceValueOut.toggle = ~faceState->faceValueOut.toggle;
      
      // Grab the first element in the queue - we'll need it either way
      CommandAndData commandAndData = commQueues[f][0];

      // Send either the command or data depending on the toggle bit
      if (faceState->faceValueOut.toggle == TOGGLE_COMMAND)
      {
        faceState->faceValueOut.value = commandAndData.command;
      }
      else
      {
        faceState->faceValueOut.value = commandAndData.data;
  
        // No longer need this comm - shift everything towards the front of the queue
        for (byte commIndex = 1; commIndex < COMM_QUEUE_SIZE; commIndex++)
        {
          commQueues[f][commIndex-1] = commQueues[f][commIndex];
        }

        // Adjust the insertion index since we just shifted the queue
        if (commInsertionIndexes[f] == 0)
        {
          // Shouldn't get here - if so something is funky
          commInsertionIndexes[f] = COMM_INDEX_OUT_OF_SYNC;
          continue;
        }
        else
        {
          commInsertionIndexes[f]--;
        }
      }
    }
  }

  FOREACH_FACE(f)
  {
    // Update the value sent in case anything changed
    sendValueOnFace(f, faceStates[f].faceValueOut);
  }
}

void processCommForFace(CommandType command, byte value, byte f)
{
  static int paramToReceive = 0;
  FaceState *faceState = &faceStates[f];

  // Special case where we are using command & value as multiple 8-bit data
  if (paramToReceive > 0)
  {
    if ((paramToReceive == 1 && tileRole == kTileRole_Param1) ||
        (paramToReceive == 2 && tileRole == kTileRole_Param2) ||
        (paramToReceive == 3 && tileRole == kTileRole_Param3))
    {
      ourValue = command << 4 | value;
    }
    paramToReceive--;
    // Don't process this like a normal command
    return;
  }

  char signedValue = value;

  bool needToSendParamValues = false;
  
  // Use the saved command value to determine what to do with the data
  byte offsetAmount = 1;
  switch (command)
  {
    case CommandType_AssignRole:
      if (tileRole == kTileRole_Unassigned)
      {
        tileRole = value;
        paletteFace = f;
      }
      break;
  
    case CommandType_SetColorSpace:
      if (tileRole == kTileRole_Param1 ||
          tileRole == kTileRole_Param2 ||
          tileRole == kTileRole_Param3)
      {
        if (colorSpace != value)
        {
          colorSpace = value;
          paramDir = kParamDir_Increment;
        }
      }
      break;

    case CommandType_IncMoreParam:
      offsetAmount <<= 2;
    case CommandType_IncParam:
      if (tileRole == kTileRole_Palette)
      {
        incParamByAmountOnFace(value, offsetAmount, 0);
        incParamByAmountOnFace(value, offsetAmount, 1);
        incParamByAmountOnFace(value, offsetAmount, 2);
        incParamByAmountOnFace(value, offsetAmount, 3);
        incParamByAmountOnFace(value, offsetAmount, 4);
        incParamByAmountOnFace(value, offsetAmount, 5);
        needToSendParamValues = true;
      }
      break;
      
    case CommandType_DecMoreParam:
      offsetAmount <<= 2;
    case CommandType_DecParam:
      if (tileRole == kTileRole_Palette)
      {
        decParamByAmountOnFace(value, offsetAmount, 0);
        decParamByAmountOnFace(value, offsetAmount, 1);
        decParamByAmountOnFace(value, offsetAmount, 2);
        decParamByAmountOnFace(value, offsetAmount, 3);
        decParamByAmountOnFace(value, offsetAmount, 4);
        decParamByAmountOnFace(value, offsetAmount, 5);
        needToSendParamValues = true;
      }
      break;

    case CommandType_SendingParams:
      paramToReceive = 3;
      break;

#if DEBUG_COMMS
    case CommandType_UpdateState:
      faceState->ourState = value;
      break;
#endif
  }

  if (needToSendParamValues)
  {
    FOREACH_FACE(f2)
    {
      sendParamValuesOnFace(f2);
    }
  }
}

void incParamByAmountOnFace(eTileRole role, byte amount, byte f)
{
  if (colorSpace == kColorSpace_RGB)
  {
    // *8 so every step has an effect
    amount <<= 3;
    
    byte *valPtr = null;
    switch (role)
    {
      case kTileRole_Param1: valPtr = &colorRGB[f].r; break;
      case kTileRole_Param2: valPtr = &colorRGB[f].g; break;
      case kTileRole_Param3: valPtr = &colorRGB[f].b; break;
    }
    
    if (0xF8 - (*valPtr & 0xF8) > amount)
    {
      *valPtr += amount;
    }
    else
    {
      *valPtr = 0xF8;
    }
  }
  else
  {
    byte *valPtr = null;
    switch (role)
    {
      case kTileRole_Param1: valPtr = &colorHSB[f].h; break;
      case kTileRole_Param2: valPtr = &colorHSB[f].s; break;
      case kTileRole_Param3: valPtr = &colorHSB[f].b; break;
    }

    // Change the given param until it has made the desired number of visible changes
    Color colorBefore = makeColorHSB(colorHSB[f].h, colorHSB[f].s, colorHSB[f].b);
    while (amount > 0)
    {
      if (role == kTileRole_Param1)
      {
        // Will overflow and wrap around, but that's okay
        *valPtr += 1;
      }
      else
      {
        if (*valPtr < 0xFF)
        {
          *valPtr += 1;
        }
        else
        {
          // Reached limit - break out
          *valPtr = 0xFF;
          break;
        }
      }
      
      Color colorAfter = makeColorHSB(colorHSB[f].h, colorHSB[f].s, colorHSB[f].b);
      if (colorAfter.r != colorBefore.r ||
          colorAfter.g != colorBefore.g ||
          colorAfter.b != colorBefore.b)
      {
        amount--;
      }
    }
  }
}

void decParamByAmountOnFace(eTileRole role, byte amount, byte f)
{
  if (colorSpace == kColorSpace_RGB)
  {
    // *8 so every step has an effect
    amount <<= 3;

    byte *valPtr = null;
    switch (role)
    {
      case kTileRole_Param1: valPtr = &colorRGB[f].r; break;
      case kTileRole_Param2: valPtr = &colorRGB[f].g; break;
      case kTileRole_Param3: valPtr = &colorRGB[f].b; break;
    }
    
    if ((*valPtr & 0xF8) > amount)
    {
      *valPtr -= amount;
    }
    else
    {
      *valPtr = 0x00;
    }
  }
  else
  {
    byte *valPtr = null;
    switch (role)
    {
      case kTileRole_Param1: valPtr = &colorHSB[f].h; break;
      case kTileRole_Param2: valPtr = &colorHSB[f].s; break;
      case kTileRole_Param3: valPtr = &colorHSB[f].b; break;
    }
    
    // Change the given param until it has made the desired number of visible changes
    Color colorBefore = makeColorHSB(colorHSB[f].h, colorHSB[f].s, colorHSB[f].b);
    while (amount > 0)
    {
      if (role == kTileRole_Param1)
      {
        // Will underflow and wrap around, but that's okay
        *valPtr -= 1;
      }
      else
      {
        if (*valPtr > 0)
        {
          *valPtr -= 1;
        }
        else
        {
          *valPtr = 0x00;
        }
      }

      Color colorAfter = makeColorHSB(colorHSB[f].h, colorHSB[f].s, colorHSB[f].b);
      if (colorAfter.r != colorBefore.r ||
          colorAfter.g != colorBefore.g ||
          colorAfter.b != colorBefore.b)
      {
        amount--;
      }
    }
  }
}

void sendParamValuesOnFace(byte f)
{
  enqueueCommOnFace(f, CommandType_SetColorSpace, colorSpace, false);
  enqueueCommOnFace(f, CommandType_SendingParams, 0, false);
  if (colorSpace == kColorSpace_RGB)
  {
    // Send in opposite order because we decrement the param number when receiving
    enqueueCommOnFace(f, colorRGB[f].b >> 4, colorRGB[f].b & 0xF, false);
    enqueueCommOnFace(f, colorRGB[f].g >> 4, colorRGB[f].g & 0xF, false);
    enqueueCommOnFace(f, colorRGB[f].r >> 4, colorRGB[f].r & 0xF, false);
  }
  else
  {
    // Send in opposite order because we decrement the param number when receiving
    enqueueCommOnFace(f, colorHSB[f].b >> 4, colorHSB[f].b & 0xF, false);
    enqueueCommOnFace(f, colorHSB[f].s >> 4, colorHSB[f].s & 0xF, false);
    enqueueCommOnFace(f, colorHSB[f].h >> 4, colorHSB[f].h & 0xF, false);
  }
}

// =================================================================================================

void assignRoles()
{
  // Delay a bit to give everyone a chance to be ready
  if (!bootTimer.isExpired())
  {
    return;
  }
  
  // Lone tiles get reset
  if (isAlone())
  {
    tileRole = kTileRole_Unassigned;
    return;
  }
  
  // If already have a role then nothing to do
  if (tileRole != kTileRole_Unassigned)
  {
    return;
  }

  // If we have three (or more) neighbors then we become a palette
  // and three neighbors become our parameters.
  byte numNeighbors = 0;
  FOREACH_FACE(f)
  {
    if (faceStates[f].neighborSyncFlags & NeighborSyncFlag_Present)
    {
      numNeighbors++;
    }
  }

  if (numNeighbors >= 3)
  {
    tileRole = kTileRole_Palette;
    colorSpace = kColorSpace_RGB;
    FOREACH_FACE(f)
    {
      colorRGB[f].r = 128;
      colorRGB[f].g = 0;
      colorRGB[f].b = 0;
    }
  }
}

void assignNewNeighborRoles()
{
  for (byte roleToAssign = kTileRole_Param1; roleToAssign < kTileRole_MAX; roleToAssign++)
  {
    // Check if this role has already been assigned
    bool shouldAssign = true;
    FOREACH_FACE(f)
    {
      if (roleOnFace[f] == roleToAssign)
      {
        shouldAssign = false;
        break;
      }
    }
    if (!shouldAssign)
    {
      continue;
    }

    // Got here so must need to assign this role
    // Try to find the next unassigned neighbor
    FOREACH_FACE(f)
    {
      if (faceStates[f].neighborSyncFlags & NeighborSyncFlag_Present)
      {
        if (roleOnFace[f] == kTileRole_Unassigned)
        {
          enqueueCommOnFace(f, CommandType_AssignRole, roleToAssign, false);
          sendParamValuesOnFace(f);
          roleOnFace[f] = roleToAssign;

          // Only assign the role once
          break;
        }
      }
    }
  }
}

void processUserInput()
{
  if (buttonSingleClicked())
  {
    switch (tileRole)
    {
      case kTileRole_Unassigned:
        break;
        
      case kTileRole_Param1:
      case kTileRole_Param2:
      case kTileRole_Param3:
        enqueueCommOnFace(paletteFace, paramDir == kParamDir_Increment ? CommandType_IncParam : CommandType_DecParam, tileRole, false);
        break;
      
      case kTileRole_Palette:
        break;
    }
  }

  if (buttonDoubleClicked())
  {
    switch (tileRole)
    {
      case kTileRole_Unassigned:
        break;
        
      case kTileRole_Param1:
      case kTileRole_Param2:
      case kTileRole_Param3:
        enqueueCommOnFace(paletteFace, paramDir == kParamDir_Increment ? CommandType_IncMoreParam : CommandType_DecMoreParam, tileRole, false);
        break;
      
      case kTileRole_Palette:
        break;
    }
  }

  if (buttonMultiClicked())
  {
    switch (tileRole)
    {
      case kTileRole_Unassigned:
        break;

      // Multi-click parameters toggles between increment and decrement
      case kTileRole_Param1:
      case kTileRole_Param2:
      case kTileRole_Param3:
        paramDir = paramDir == kParamDir_Increment ? kParamDir_Decrement : kParamDir_Increment;
        break;

      // Multi-click palette toggles between color spaces
      // Try to keep the same color. Going from HSB to RGB is easy, but RGB to HSB might be inaccurate.
      case kTileRole_Palette:
        colorSpace = colorSpace == kColorSpace_RGB ? kColorSpace_HSB : kColorSpace_RGB;

        // Convert color between spaces
        FOREACH_FACE(f)
        {
          if (colorSpace == kColorSpace_RGB)
          {
            Color color = makeColorHSB(colorHSB[f].h, colorHSB[f].s, colorHSB[f].b);
            colorRGB[f].r = color.r << 3;
            colorRGB[f].g = color.g << 3;
            colorRGB[f].b = color.b << 3;
          }
          else
          {
            rgbToHsbOnFace(f);
          }
        }

        FOREACH_FACE(f)
        {
          if (faceStates[f].neighborSyncFlags & NeighborSyncFlag_Present)
          {
            sendParamValuesOnFace(f);
          }
        }
        break;
    }
  }
}

// Pinched and adapted from:
// https://stackoverflow.com/questions/3018313/algorithm-to-convert-rgb-to-hsv-and-hsv-to-rgb-in-range-0-255-for-both
void rgbToHsbOnFace(byte f)
{
  RGB *rgb = &colorRGB[f];
  HSB *hsb = &colorHSB[f];
  
  unsigned char rgbMin, rgbMax;

  rgbMin = rgb->r < rgb->g ? (rgb->r < rgb->b ? rgb->r : rgb->b) : (rgb->g < rgb->b ? rgb->g : rgb->b);
  rgbMax = rgb->r > rgb->g ? (rgb->r > rgb->b ? rgb->r : rgb->b) : (rgb->g > rgb->b ? rgb->g : rgb->b);

  hsb->b = rgbMax;
  if (hsb->b == 0)
  {
      hsb->h = 0;
      hsb->s = 0;
      return;
  }

  hsb->s = 255 * long(rgbMax - rgbMin) / hsb->b;
  if (hsb->s == 0)
  {
      hsb->h = 0;
      return;
  }

  if (rgbMax == rgb->r)
      hsb->h = 0 + 43 * (rgb->g - rgb->b) / (rgbMax - rgbMin);
  else if (rgbMax == rgb->g)
      hsb->h = 85 + 43 * (rgb->b - rgb->r) / (rgbMax - rgbMin);
  else
      hsb->h = 171 + 43 * (rgb->r - rgb->g) / (rgbMax - rgbMin);
}

// =================================================================================================
//
// RENDER
//
// =================================================================================================

void render()
{
  setColor(OFF);

  byte paramDimness = paramDir == kParamDir_Increment ? 255 : 128;

  if (colorSpace == kColorSpace_RGB)
  {
    switch (tileRole)
    {
      case kTileRole_Palette:
        FOREACH_FACE(f)
        {
          setColorOnFace(makeColorRGB(colorRGB[f].r, colorRGB[f].g, colorRGB[f].b), f);
        }
        break;
  
      case kTileRole_Param1:
        setColorOnFace(dim(RED, paramDimness), paletteFace);
        // Output bitwise R value on remaining 5 faces
        if (ourValue & (1<<7)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 1));
        if (ourValue & (1<<6)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 2));
        if (ourValue & (1<<5)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 3));
        if (ourValue & (1<<4)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 4));
        if (ourValue & (1<<3)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 5));
        break;
      case kTileRole_Param2:
        setColorOnFace(dim(GREEN, paramDimness), paletteFace);
        if (ourValue & (1<<7)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 1));
        if (ourValue & (1<<6)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 2));
        if (ourValue & (1<<5)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 3));
        if (ourValue & (1<<4)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 4));
        if (ourValue & (1<<3)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 5));
        break;
      case kTileRole_Param3:
        setColorOnFace(dim(BLUE, paramDimness), paletteFace);
        if (ourValue & (1<<7)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 1));
        if (ourValue & (1<<6)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 2));
        if (ourValue & (1<<5)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 3));
        if (ourValue & (1<<4)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 4));
        if (ourValue & (1<<3)) setColorOnFace(dim(WHITE, 128), CCW_FROM_FACE(paletteFace, 5));
        break;
    }
  }
  else
  {
    switch (tileRole)
    {
      case kTileRole_Palette:
        FOREACH_FACE(f)
        {
          setColorOnFace(makeColorHSB(colorHSB[f].h, colorHSB[f].s, colorHSB[f].b), f);
        }
        break;
  
      case kTileRole_Param1:
        setColorOnFace(dim(RED,     paramDimness), 0);
        setColorOnFace(dim(ORANGE,  paramDimness), 1);
        setColorOnFace(dim(YELLOW,  paramDimness), 2);
        setColorOnFace(dim(GREEN,   paramDimness), 3);
        setColorOnFace(dim(BLUE,    paramDimness), 4);
        setColorOnFace(dim(MAGENTA, paramDimness), 5);
        break;
      case kTileRole_Param2:
        setColorOnFace(dim(makeColorHSB(0, 255, 255), paramDimness), 0);
        setColorOnFace(dim(makeColorHSB(0, 208, 255), paramDimness), 1);
        setColorOnFace(dim(makeColorHSB(0, 160, 255), paramDimness), 2);
        setColorOnFace(dim(makeColorHSB(0, 104, 255), paramDimness), 3);
        setColorOnFace(dim(makeColorHSB(0,  72, 255), paramDimness), 4);
        setColorOnFace(dim(makeColorHSB(0,  40, 255), paramDimness), 5);
        break;
      case kTileRole_Param3:
        setColor(dim(WHITE, paramDimness));
        break;
    }
  }
  
  // Error codes
  FOREACH_FACE(f)
  {
    if (ErrorOnFace(f))
    {
      setColorOnFace(makeColorRGB(255,0,0), f);
    }
  }

#if DEBUG_COMMS
  FOREACH_FACE(f)
  {
    FaceState *faceState = &faceStates[f];

    if (ErrorOnFace(f))
    {
      if (commInsertionIndexes[f] == COMM_INDEX_ERROR_OVERRUN)
      {
        setColorOnFace(MAGENTA, f);
      }
      else if (commInsertionIndexes[f] == COMM_INDEX_OUT_OF_SYNC)
      {
        setColorOnFace(ORANGE, f);
      }
      else if (commInsertionIndexes[f] == COMM_DATA_OVERRUN)
      {
        setColorOnFace(RED, f);
      }
      else
      {
        setColorOnFace(GREEN, f);
      }
    }
    else if (!isValueReceivedOnFaceExpired(f))
    {
      if (faceState->ourState == 2)
      {
        setColorOnFace(GREEN, f);
      }
      else if (faceState->ourState == 3)
      {
        setColorOnFace(BLUE, f);
      }
    }
  }
#endif

}
