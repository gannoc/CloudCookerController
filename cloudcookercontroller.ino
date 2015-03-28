#include <EthernetClient.h>
#include <EthernetUdp.h>
#include <Dns.h>
#include <SPI.h>
#include <EthernetServer.h>
#include <Ethernet.h>
#include <Dhcp.h>
#include <EEPROM.h>
#include <TextFinder.h>

//Define identifiers for pins
#define NETSTATREDPIN 5
#define NETSTATGREENPIN 6
#define NETSTATBLUEPIN 3
#define PAIRBUTTON A0

//Delays between updates to Azure.
#define AZUREUPDATEDELAY 30


//RGB Light Globals
//Created a struct for RGB values to simplify setting LED colors in code.
struct RGB {
  int red;
  int green;
  int blue;
  RGB& operator =(const RGB& a)
    {
    red = a.red;
    green = a.green;
    blue = a.blue;
    return *this;
    }
};
//Pre-Defined colors.
const RGB colorRed = {255,0, 0};
const RGB colorGreen =  {0, 255, 0};
const RGB colorYellow = {255, 255, 0};
const RGB colorWhite =  {255, 255, 255};
const RGB colorBlue =  {0, 0, 255};

//Global Value of network status indicator light
RGB netstatColor = {0,0,0};

//
//  Mac address
//
byte mac[] = {  
  0x90, 0xA2, 0xDA, 0x0F, 0x59, 0x1E };

//Global Ethernet Client, and TextFinder for the client stream.
EthernetClient client;
TextFinder finder( client );

//Global buffer 
char buffer[256]="";

// Azure Service Hostname and appkey.
char MobileServiceHostName[] = "YOURSERVICENAME-code.azurewebsites.net"; 
char MS_ApplicationKey[33] = "APPLICATIONKEY";


// Variables for Serial Number of device and shared secret.
// These will be different for each device and stored in eeprom.
char serialNumber[16]= ""; 
char sharedSecret[33]= "";




//Global status variables for cooker.
//These values are set by Azure service.
char currentCookConfigurationID[33]="";
int setpointTemperature = 0;
int targetFoodTemperature = 0;
//These values are set by arduino using thermoprobes and PID algorithm
int currentTemperature = 0;
int currentFoodTemperature = 0;
bool controlElementActive = false;  //is fan on

// long uint used for timing.
unsigned long updatetimer = 0;

//Function to set the Network StatusID to the given color.
void setNetworkStatusLED(struct RGB c)
{
    netstatColor = c;
    analogWrite(NETSTATREDPIN, netstatColor.red);
    analogWrite(NETSTATGREENPIN, netstatColor.green);       
    analogWrite(NETSTATBLUEPIN, netstatColor.blue);
}

//Initialization function to load the serial number and security key from EEPROM.
void loadSerialandSharedSecretFromEEPROM()
{
  Serial.println("Loading keys from EEPROM");
  for (int e = 0;e<15;e++)
  {
    serialNumber[e] = EEPROM.read(e);
  }
  for (int e = 15;e<47;e++)
  {
    sharedSecret[e-15]=EEPROM.read(e);
  }
  Serial.print("Serial Number = ");  
  Serial.println(serialNumber);
  Serial.print("Shared Secret = ");  
  Serial.println(sharedSecret);
}


//The setup function is run once on arduino startup or reset.
void setup() {

  //Set up serial interface for debugging.
  Serial.begin(9600);
  while (!Serial) { }

  //Configure Arduino pins
  pinMode(NETSTATREDPIN, OUTPUT);
  pinMode(NETSTATGREENPIN, OUTPUT);
  pinMode(NETSTATBLUEPIN, OUTPUT);
  pinMode(PAIRBUTTON, INPUT_PULLUP);
 
  //Set Initial Status color to white.
  setNetworkStatusLED(colorWhite);
  //Load serial number and shared secret from eeprom  
  loadSerialandSharedSecretFromEEPROM();

  // Init the Ethernet connection:
  if (Ethernet.begin(mac) == 0) 
  {
    Serial.println("Could not init ethernet.  Connection not found or DHCP error.");
    setNetworkStatusLED(colorRed);
    while(true);
  }
  Serial.println("Network connected.");

  //Set color to yellow.
  setNetworkStatusLED(colorYellow);
  //Do an initial update.  Further updates will be called from the loop function.
  doCloudCookerUpdate();
  
}

//This function is called every X seconds to update Azure with the current cooking values.
void doCloudCookerUpdate()
{  
    if (client.connect(MobileServiceHostName, 80)) 
    {
      Serial.println("Connected to Azure");
      //Manually create JSON in Buffer for POST Request.  This is much less expensive than using
      //JSON Libraries.
      sprintf(buffer, "{");
      sprintf(buffer + strlen(buffer), "\"serialNumber\": \"%s\",",serialNumber);
      sprintf(buffer + strlen(buffer), "\"sharedSecret\": \"%s\",",sharedSecret);
      sprintf(buffer + strlen(buffer), "\"cookConfigurationID\": \"%s\",",currentCookConfigurationID);
      sprintf(buffer + strlen(buffer), "\"currentTemperature\": \"%d\",",currentTemperature);
      sprintf(buffer + strlen(buffer), "\"currentFoodTemperature\": \"%d\",",currentFoodTemperature);
      sprintf(buffer + strlen(buffer), "\"controlElementActive\": \"%s\"",controlElementActive ? "true" : "false");
      sprintf(buffer + strlen(buffer), "}");
      //

      // HTTP REQUEST
      
      client.println("POST /api/DirectUpdate HTTP/1.1");
      client.print("Host: ");
      client.println(MobileServiceHostName);      
      //ZUMO apparently stands for aZUre MObile services.
      client.print("X-ZUMO-APPLICATION: ");
      client.println(MS_ApplicationKey);

      client.println("Content-Type: application/json");
      
    // Content length
      client.print("Content-Length: ");
      client.println(strlen(buffer));
 
    // End of headers
      client.println();
 
    // Request body
      client.println(buffer);
      
      // HTTP RESPONSE
      
      //Search for HTTP status code
      finder.find("HTTP/1.1");
      int statuscode = finder.getValue();
      
      Serial.print("Statuscode = ");
      Serial.println(statuscode);
      //Depending on status code, set the appropriate LED color and optionally continue to read the response.
      switch(statuscode)
      {
        case 200:
            //This should not happen unless the service is malfunctioning.
            setNetworkStatusLED(colorYellow);            
            client.stop();
            break;
          case 201:
            //Update received.
            setNetworkStatusLED(colorGreen);     
            client.stop();
            break;
          case 204:
            //Device is not configured for a cookconfiguration.         
            setNetworkStatusLED(colorWhite);
            currentCookConfigurationID[0]=0;
            setpointTemperature=0;
            targetFoodTemperature=0;
            client.stop();
            break;        
          case 250:
            //Device has a new configuration.
            
            //This uses the TextFinder library to load values from the returned JSON object.  
            //This code is fragile, but has the smallest memory footprint.
            finder.find("\"CookConfigurationID\"");
            finder.getString("\"","\"",currentCookConfigurationID,200);
            setpointTemperature = finder.getValue();
            targetFoodTemperature= finder.getValue();
            
            client.stop();
            break;        
          case 403:
            //Unrecognized device or invalid shared secret.
            setNetworkStatusLED(colorBlue);
            currentCookConfigurationID[0]=0;
            setpointTemperature=0;
            targetFoodTemperature=0;     
            client.stop();
            break;                
          default:
             //All other responses are bad.
            setNetworkStatusLED(colorRed); 
            client.stop();
      }
    } 
    else 
    {
    Serial.println("Connection to Azure Failed");
    setNetworkStatusLED(colorRed);
    }
}

//Not implemented yet.
void doTemperatureReadings()
{
  
  //currentTemperature=eggProbe.readFahrenheit();    
  //currentFoodTemperature=foodProbe.readFahrenheit();
  
}

//Not implemented yet.
void doPIDCompute()
{
  //Not implemented
  //cookerPID is linked to currentTemperate, setpoint, and fan.
  //cookerPID.Compute();
  //if (Fanspeed>X) controlElementActive=true else controlElementActive=false;
  //analogWrite(FANPIN,Output);
}

void loop() 
{
  //Will call doCloudCookerUpdate every 30 seconds.
  if (millis()>updatetimer+1000*AZUREUPDATEDELAY)
  {
    doCloudCookerUpdate();
    updatetimer=millis();
  }
  
  doTemperatureReadings();
  doPIDCompute();
  
 
}

