/*
  simple_example

  This code connects and initializes one shield (with address set to 000, otherwise change the SHIELD_ADRESS value to 1, 2, 3, etc...)
  It sets the solenoid outputs states to LOW, and once setup is done, it cycles through the outputs and sets them to HIGH sequentially
*/

// Make sure you installed the Adafruit MCP23017 library 
// (Sketch > Include Library > Manage Libraries... > *search for MCP23017, select Adafruit one* > Install)
#include <Wire.h>
#include <MCP23017.h>
#include <string.h>

#define MCP23017_ADDR 0x21 // SHIELD/MCP23017 address (change to 1-8 if soldered differently)
#define BAUD_RATE 9600
#define PUMP_MIN 100 // Minimum duration for the "on" state of the Lee Co. LPM pumps

String inString = String(100);  
char strValue[12];

MCP23017 mcp = MCP23017(MCP23017_ADDR);

// State variables
word mcpstate = 0;
word mcpstatePUM = 0;

// PUMP structure:
struct PUMP
{
    int pin;
    long period = 200; // in ms, 200 or more
    int channel;
    // bool isonPUM = 0; // -JS
    bool active = 0;
    unsigned long lastswitch = 0; // The time will overflow after about 50 days...
};
// For now it is just an empty array. The array will be updated dynamically during runtime.
PUMP* PUMParr = 0;
int PUMParrsize = 0;

// Valve structure:
struct VALVE
{
    int pin;
    int channel;
    bool ison = 0;
    bool PWMon = 0;
    int PWMperc = 50; // PWM percentage
    long period = 10000; // in ms, 10 or more (preferrably on the order of seconds)
    unsigned long lastswitch = 0; // The time will overflow after about 50 days...
};

// For now it is just an empty array. The array will be updated dynamically during runtime.
VALVE* VALVEarr = 0;
int VALVEarrsize = 0;

// Set structure
struct PumpValveSet {
    int pumpIndex; //Pump
    int valveIndices[3]; //Three valves
    bool active; // If it's on or not
    int currentStep = 0; // Start at first stage of sequence
    unsigned long lastStepTime = 0;
};
//Initialize
PumpValveSet* sets = 0;
int setCount = 0;

void readAndPrintPorts() {
    byte portAState = mcp.readRegister(MCP23017Register::GPIO_A);
    byte portBState = mcp.readRegister(MCP23017Register::GPIO_B);

    Serial.print("Port A: ");
    for (int i = 0; i < 8; i++) {
        Serial.print(bitRead(portAState, i));
        Serial.print(" ");
    }
    Serial.println();

    Serial.print("Port B: ");
    for (int i = 0; i < 8; i++) {
        Serial.print(bitRead(portBState, i));
        Serial.print(" ");
    }
    Serial.println();
}

void setup()
{
  Wire.begin();
  // Intialize serial connection and LED pin
  Serial.println("Starting up...");
  Serial.begin(BAUD_RATE);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN,HIGH);

  // Connect to MCP
  Serial.println("Initializing i2c for mcp23017 comm...");
  mcp.init();

  // Set MCP pins as outputs
  Serial.println("Setting up MCP pins to outputs...");
  mcp.portMode(MCP23017Port::A, 0x00); // Set all pins on port A as output
  mcp.portMode(MCP23017Port::B, 0x00);
  // Write init state to MCP
  Serial.println("Setting up MCP pins init state...");
  applystate();

  // Initialize valves:
  for (int i = 0; i<16; i++)
  {
    createnewValve(i, i+1, 0, 0, 50, 5000);
  }
  // Initialize pumps:
  for (int i = 0; i<8; i++)
  {
    createnewPump(8+i, 200, i+1, 0);
  }
  Serial.println("Setup done!");
}

void loop() {
  //Initialize index, string, and time
  int indcmd = -1;
  inString = String(100);
  unsigned long now = millis();

// Update the state each time through the loop
  updatestate();

// When there are sets, have them loop through the sequence of states -JS
for (int i = 0; i < setCount; i++) {
    PumpValveSet& set = sets[i];
    // If the set or pump for the set is not active then skip
    if (!set.active || !PUMParr[set.pumpIndex].active) continue;
    // Loop through the sequence using the given period
    long period = PUMParr[set.pumpIndex].period;
    if (now - set.lastStepTime >= period) {
      const byte loopArray[6][3] = {
        {1, 0, 1}, {1, 0, 0}, {1, 1, 0},
        {0, 1, 0}, {0, 1, 1}, {0, 0, 1}
      };
      // Turn the correct valves on and off
      for (int v = 0; v < 3; v++) {
        int valveIdx = set.valveIndices[v];
        if (valveIdx >= 0 && valveIdx < VALVEarrsize) {
          VALVEarr[valveIdx].ison = loopArray[set.currentStep][v];
        }
      }
      //Go to next step
      set.currentStep = (set.currentStep + 1) % 6;
      set.lastStepTime = now;
    }
  }

  // Anything on the serial?
  while (Serial.available()) 
  {
    delay(10);  //small delay to allow input buffer to fill
    if (Serial.available() >0) 
    {
        char c = Serial.read();  //gets one byte from serial buffer
        if (c == ';') {break;}  //breaks out of capture loop to print readstring
        inString += c; //makes the string inString
    }
  }

  // VAL command? (Set the valves state) (Note: If the valve is in PWM state, this will be overidden by the PWM subroutine)
  //VALXXXSX (XXX = valve channel, X = state)
  indcmd = inString.indexOf("VAL"); // Finds the VAL command start sequence (set valve)
  if(indcmd >= 0)
  {
      //Serial.print("Here\n");
      int i = findValve((int)inString.substring(indcmd+3,indcmd+6).toInt());
      //Serial.print("i=\n");
      //Serial.print(i);
      //Serial.print("\n");

      if(i>=0)
      {
        VALVEarr[i].ison = (bool)inString.substring(indcmd+7,indcmd+8).toInt();
      } 
      
  }

  // PUM command? (Set the pump state)  -JS 
  // PUMXXXSXVXXXVXXXVXXXP[XXX];(XXX = pump channel, X = state, VXXX = 3 valves channels, [XXX] = period (ms))
  indcmd = inString.indexOf("PUM"); // Finds the PUM command start sequence (set pump)

  // If the command is short (does not assign valves to a set) then assign change
  // This is useful when the user pushes the "pump off button" to turn off the whole set
  if(indcmd >= 0 && inString.length() >= 6 && inString.length() <= 19){
    //Turn pump on or off
    int i = findPump((int)inString.substring(indcmd+3,indcmd+6).toInt());
    //If pump is turned off, then turn off the valves in its set
    if(i>=0){
        bool newState = (bool)inString.substring(indcmd+7,indcmd+8).toInt();
        PUMParr[i].active = newState;
        if (!newState) {
          // Turn off valves and remove the set
          for (int s = 0; s < setCount; s++) {
            if (sets[s].pumpIndex == i) {
              for (int v = 0; v < 3; v++) {
                int valveIdx = sets[s].valveIndices[v];
                if (valveIdx >= 0 && valveIdx < VALVEarrsize) {
                  VALVEarr[valveIdx].ison = 0;
                  VALVEarr[valveIdx].PWMon = 0;}}}}
          // If the pump is turned off, remove its set
          removeSetsWithPump(i); 
  }

      }
  } //If it is a full command call this fxn
    // We don't need to worry about turning off the set on a full command
  else if(indcmd >= 0)
  {
    parsePUMCommand(inString);  
  }
  


  // PWM command? (Set valve in pwm mode)
  //PWMXXXSX%XXXP[XXX] (XXX = pump channel, X = state, XXX = duty cycle percentage, [XXX] = period (ms))
  indcmd = inString.indexOf("PWM"); // Finds the PUM command start sequence (set pump)
  if(indcmd >= 0)
  {
      int i = findValve((int)inString.substring(indcmd+3,indcmd+6).toInt());

      if(i>=0)
      {
        VALVEarr[i].PWMon = (bool)inString.substring(indcmd+7,indcmd+8).toInt();
        VALVEarr[i].PWMperc = (int)inString.substring(indcmd+9,indcmd+12).toInt();
        VALVEarr[i].period = inString.substring(indcmd+13).toInt();
        if(VALVEarr[i].period<10) VALVEarr[i].period = 10; // Period must be >= 10ms
      }
  }



  }//End of void loop()



void updatestate()
{
    // Initialize temp change variables
    word newstate = mcpstate;
    word newstatePUM = mcpstatePUM;
    unsigned long now = millis();
    long elapsed;

    // Run through pumps:
    for (int i = 0; i<PUMParrsize;i++)
    {
      elapsed = now - PUMParr[i].lastswitch;
      if (!PUMParr[i].active)
      {
        bitClear(newstate,PUMParr[i].pin);
        PUMParr[i].lastswitch = now;
      }
      else if (elapsed > PUMParr[i].period)
      {
        bitSet(newstate,PUMParr[i].pin);
        PUMParr[i].lastswitch = now;
      }
      else if (elapsed < 0) // if the millis() long overflows, reset
      {
        bitSet(newstate,PUMParr[i].pin);
        PUMParr[i].lastswitch = now;
      }
      else if (elapsed < PUMP_MIN)
      {
        bitSet(newstate,PUMParr[i].pin);
      }
      else
      {
        bitClear(newstate,PUMParr[i].pin);
      }
    }

    // Run through valve PWMs:
    for (int i = 0; i<VALVEarrsize;i++)
    {
      elapsed = now - VALVEarr[i].lastswitch;
      if (!VALVEarr[i].PWMon)
      {
        VALVEarr[i].lastswitch = now;
      }
      else if (elapsed > VALVEarr[i].period)
      {
        VALVEarr[i].ison = 1;
        VALVEarr[i].lastswitch = now;
      }
      else if (elapsed < 0) // if the millis() long overflows, reset
      {
        VALVEarr[i].ison = 1;
        VALVEarr[i].lastswitch = now;
      }
      else if (elapsed < (VALVEarr[i].period*VALVEarr[i].PWMperc)/100)
      {
        VALVEarr[i].ison = 1;
      }
      else
      {
        VALVEarr[i].ison = 0;
      }
      
    }
    
    // Run through the valves states: 
    for (int i = 0; i<VALVEarrsize;i++)
    {
        if (VALVEarr[i].ison)
        {
          bitSet(newstate,VALVEarr[i].pin);
        }
        else
        {
          bitClear(newstate,VALVEarr[i].pin);
        }
    }

    // Run through the pumps states to turn them on or off -JS
    for (int i = 0; i<PUMParrsize;i++)
    {
        if (PUMParr[i].active)
        {
          bitSet(newstatePUM,PUMParr[i].pin);
        }
        else
        {
          bitClear(newstatePUM,PUMParr[i].pin);
        }
    }
          
    if (newstate!=mcpstate) // only update if necessary
    {
        mcpstate = newstate;
        applystate();
        
    }
    if (newstatePUM!=mcpstatePUM) // Update current pump array if necessary -JS
    {
        mcpstatePUM = newstatePUM;
        Serial.print("Pump States:\n");
        printstate(mcpstatePUM); // For debugging
        //applystate(); // do we need a version of this for the pumps?


        
    }

}


void applystate()
{
  Serial.print("Valve States:\n");
  printstate(mcpstate); // For debugging
    // Get the lower 8 bits (first 8 bits)
  uint8_t lowByte = mcpstate & 0xFF;

  // Get the upper 8 bits (next 8 bits)
  uint8_t highByte = (mcpstate >> 8) & 0xFF;

  // Serial.print("First 8 bits (Low Byte): 0x");
  // Serial.println(lowByte, HEX);

  // Serial.print("Second 8 bits (High Byte): 0x");
  // Serial.println(highByte, HEX);
  mcp.writeRegister(MCP23017Register::GPIO_A, lowByte);
  mcp.writeRegister(MCP23017Register::GPIO_B, highByte);
  //readAndPrintPorts();
}

void printstate(word state)   
{ 
    for (word mask = 0x8000; mask; mask >>= 1)
    {
      Serial.print(mask&state?'1':'0');
    }
    Serial.println();
}   

void createnewValve(int pin, int channel, bool ison, bool PWMon, int PWMperc, long period)
{
    VALVEarrsize++;
    // Allocation or re-allocation
    if (VALVEarr != 0)
    {
        VALVEarr = (VALVE*) realloc(VALVEarr, VALVEarrsize * sizeof(VALVE));
    }
    else
    {
        VALVEarr = (VALVE*) malloc(VALVEarrsize * sizeof(VALVE));
    }
    Serial.print("Creating valve number: ");
    Serial.println(VALVEarrsize);

    VALVEarr[VALVEarrsize-1].pin = pin ;
    VALVEarr[VALVEarrsize-1].channel = channel ;
    VALVEarr[VALVEarrsize-1].ison = ison ;
    VALVEarr[VALVEarrsize-1].PWMon = PWMon;
    VALVEarr[VALVEarrsize-1].PWMperc = PWMperc ;
    VALVEarr[VALVEarrsize-1].period = period ;
    VALVEarr[VALVEarrsize-1].lastswitch = millis();
}

int findValve(int channel)
{
    // Find the VALVE struct to update in the array. (Only one)
    for (int i = 0; i<VALVEarrsize; i++)
    {
        if (VALVEarr[i].channel==channel)
        {
            return i;
        }
    }
    return -1;
}

void createnewPump(int pin, long period, int channel, bool active)
{
    PUMParrsize++;
    // Allocation or re-allocation
    if (PUMParr != 0)
    {
        PUMParr = (PUMP*) realloc(PUMParr, PUMParrsize * sizeof(PUMP));
    }
    else
    {
        PUMParr = (PUMP*) malloc(PUMParrsize * sizeof(PUMP));
    }

    Serial.print("Creating pump number: ");
    Serial.println(PUMParrsize);

    PUMParr[PUMParrsize-1].pin = pin;
    PUMParr[PUMParrsize-1].period = period;
    PUMParr[PUMParrsize-1].channel = channel ;
    PUMParr[PUMParrsize-1].active = active;
    PUMParr[PUMParrsize-1].lastswitch = millis();
}

int findPump(int channel)
{
    // Find the PUMP struct to update in the array. (Only one)
    for (int i = 0; i<PUMParrsize; i++)
    {
        if (PUMParr[i].channel==channel)
        {
            return i;
        }
    }
    return -1;
}
// Fxn to make a new valve set -JS
void createnewSET(int pumpIndex, int valveIndices[3], bool active) {
  setCount++;
  //Initialize set or add new set
  if (sets != nullptr) {
    sets = (PumpValveSet*) realloc(sets, setCount * sizeof(PumpValveSet));
  } else {
    sets = (PumpValveSet*) malloc(setCount * sizeof(PumpValveSet));
  }
// Link the pump and 3 valves, and turn set on
  PumpValveSet& newSet = sets[setCount - 1];
  newSet.pumpIndex = pumpIndex;
  for (int i = 0; i < 3; i++) {
    newSet.valveIndices[i] = valveIndices[i];
  }
  newSet.active = active;
  newSet.currentStep = 0;
  newSet.lastStepTime = millis();
  // Print when created
  Serial.print("Created set #");
  Serial.println(setCount);
}
// Fxn to parse PUM command and create the appropriate set -Js
void parsePUMCommand(String cmd) {
  int index = 0;
  // Pull out the pump, state, 3 valves, and the period
  // Format == PUMXXXSXVXXXVXXXVXXXP[XXX];
  while ((index = cmd.indexOf("PUM", index)) >= 0) {
    int pumpChan = cmd.substring(index + 3, index + 6).toInt();
    int state = cmd.substring(index + 7, index + 8).toInt();
    int v1 = cmd.substring(index + 9, index + 12).toInt();
    int v2 = cmd.substring(index + 13, index + 16).toInt();
    int v3 = cmd.substring(index + 17, index + 20).toInt();
    int period = cmd.substring(index + 22, index + 25).toInt();
    int pi = findPump(pumpChan);
    int vi1 = findValve(v1);
    int vi2 = findValve(v2);
    int vi3 = findValve(v3);

    // Remove pump set incase of overwrite
    if(pi>=0){
        // Turn off valves and remove the set
        for (int s = 0; s < setCount; s++) {
          if (sets[s].pumpIndex == pi) {
            for (int v = 0; v < 3; v++) {
              int valveIdx = sets[s].valveIndices[v];
              if (valveIdx >= 0 && valveIdx < VALVEarrsize) {
                VALVEarr[valveIdx].ison = 0;
                VALVEarr[valveIdx].PWMon = 0;}}}}
        // If the pump is turned off, remove its set
        removeSetsWithPump(pi); 
  } 

    //Turn pump on initialize period (min 200 ms)
    if (pi >= 0) {
        PUMParr[pi].active = state;
        PUMParr[pi].period = max(200, period);
    }
    //Create new "set" with the pump and 3 valves
    int valveSet[3] = {vi1, vi2, vi3};
    createnewSET(pi, valveSet, true);
    //Incase multiple commands at once
    index += 25;
  }
}
// Fxn to remove a set after it is turned off so the pump can be reassigned later - JS
void removeSetsWithPump(int pumpIndex) {
  int writeIndex = 0;
  // If it's not the set we want to delete, keep it
  for (int readIndex = 0; readIndex < setCount; readIndex++) {
    if (sets[readIndex].pumpIndex != pumpIndex) {
      sets[writeIndex++] = sets[readIndex];
    }
  }
  // Remove unwanted set and reduce array size
  if (writeIndex != setCount) {
    setCount = writeIndex;
    sets = (PumpValveSet*) realloc(sets, setCount * sizeof(PumpValveSet));
    Serial.print("Removed set(s) for pump ");
    Serial.println(pumpIndex + 1);
  }
}
