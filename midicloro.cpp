//************** MIDIcloro **************
//
// MIDI clock generator and router
//
// Copyright (c) 2015 David Ramstrom
// This project is licensed under the terms of the MIT license
//
// Run:
// ./midicloro [-c to start interactive configuration]
//
//***************************************

#include <iostream>
#include <fstream>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
#include <algorithm>
#include <map>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <boost/utility/binary.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/program_options.hpp>
#include <boost/regex.hpp>
#include <boost/random.hpp>
#include "rtmidi/RtMidi.h"

using namespace std;

namespace po = boost::program_options;
namespace convert {
    template<typename T> string to_string(const T& n) {
        ostringstream stm;
        stm << n;
        return stm.str();
    }
}

enum Chord {
  CHORD_OFF = 0,
  MINOR3,
  MAJOR3,
  MINOR3_LO,
  MAJOR3_LO,
  MINOR2,
  MAJOR2,
  M7,
  MAJ7,
  M9,
  MAJ9,
  SUS4,
  POWER2,
  POWER3,
  OCTAVE2,
  OCTAVE3
};

enum Velo {
  VEL_OFF,
  VEL_ON,
  VEL_RDM
};

RtMidiIn *midiin1 = 0;
RtMidiIn *midiin2 = 0;
RtMidiIn *midiin3 = 0;
RtMidiIn *midiin4 = 0;
RtMidiOut *midiout1 = 0;
RtMidiOut *midiout2 = 0;
RtMidiOut *midiout3 = 0;
RtMidiOut *midiout4 = 0;
bool done;
int sleepUSec;
bool enableClock;
bool resetClock;
bool ignoreProgramChanges;
int tempoMidiCC;
int chordMidiCC;
int routeMidiCC;
int startMidiCC;
int stopMidiCC;
int velocityMidiCC;
int bpmOffsetForMidiCC;
long clockInterval; // Clock interval in ns
long tapTempoMinInterval; // Tap-tempo min interval in ns
long tapTempoMaxInterval; // Tap-tempo max interval in ns
int velocityRandomOffset;
bool velocityMultiDeviceCtrl;
boost::mt19937 *randomGenerator;
vector<unsigned char> *clockMessage;
vector<unsigned char> *clockStartMessage;
vector<unsigned char> *clockStopMessage;
vector<unsigned char> *noteOffMessage;
int lastNote[4][16] = {{-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
                       {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
                       {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1},
                       {-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1}};
int channelRouting[4][16] = {{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
                             {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
                             {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
                             {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}};
int chordModes[4][16] = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
                         {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
                         {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
                         {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}};
int velocityModes[4][16] = {{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
                            {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
                            {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
                            {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}};
int velocity[4][16] = {{100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100},
                       {100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100},
                       {100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100},
                       {100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100}};
bool mono[4] = {false,false,false,false};
bool monoLegato[4][16] = {{false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false},
                          {false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false},
                          {false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false},
                          {false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,false}};
boost::circular_buffer<struct timespec> *tapTempoTimes;
const char *CONFIG_FILE = "midicloro.cfg";

void usage(void);
static void finish( int /*ignore*/ ){ done = true; }
double random01();
bool ignoreMessage(unsigned char msgByte);
void transposeAndSend(vector<unsigned char> *message, int semiNotes);
void sendNoteOrChord(vector<unsigned char> *message, int source);
void sendNoteOffAndNote(vector<unsigned char> *message, int source);
void setChordMode(int source, int channel, int value);
void routeChannel(vector<unsigned char> *message, int source);
void setChannelRouting(int source, int channel, int newChannel);
void applyVelocity(vector<unsigned char> *message, int source);
void setVelocityMode(int source, int channel, int value);
void setVelocityModeMulti(int source, int channel, int value);
int scaleUp(int value);
long tapTempo();
void handleMessage(vector<unsigned char> *message, int source);
void messageAtIn1(double deltatime, vector<unsigned char> *message, void */*userData*/);
void messageAtIn2(double deltatime, vector<unsigned char> *message, void */*userData*/);
void messageAtIn3(double deltatime, vector<unsigned char> *message, void */*userData*/);
void messageAtIn4(double deltatime, vector<unsigned char> *message, void */*userData*/);
string trimPort(bool doTrim, const string& str);
bool openInputPort(RtMidiIn *in, string port);
bool openOutputPort(RtMidiIn *in, string port);
bool openPorts(string i1, string i2, string i3, string i4, string o1, string o2, string o3, string o4);
void cleanUp();
void runInteractiveConfiguration();

int main(int argc, char *argv[]) {
  try {
    if (argc == 2 && string(argv[1]) == "-c")
      runInteractiveConfiguration();
    else if (argc > 1)
      usage();

    // Handle configuration
    string input1, input2, input3, input4, output, output2, output3, output4;
    bool input1mono, input2mono, input3mono, input4mono;
    int initialBpm, tapTempoMinBpm, tapTempoMaxBpm;

    po::options_description desc("Options");
    desc.add_options()
      ("input1", po::value<string>(&input1), "input1")
      ("input1mono", po::value<bool>(&input1mono)->default_value(false), "input1mono")
      ("input2", po::value<string>(&input2), "input2")
      ("input2mono", po::value<bool>(&input2mono)->default_value(false), "input2mono")
      ("input3", po::value<string>(&input3), "input3")
      ("input3mono", po::value<bool>(&input3mono)->default_value(false), "input3mono")
      ("input4", po::value<string>(&input4), "input4")
      ("input4mono", po::value<bool>(&input4mono)->default_value(false), "input4mono")
      ("output1", po::value<string>(&output), "output1")
	  ("output2", po::value<string>(&output2), "output2")
	  ("output3", po::value<string>(&output3), "output3")
	  ("output4", po::value<string>(&output4), "output4")
	  ("sleepUSec", po::value<int>(&sleepUSec)->default_value(100), "sleepUSec")
      ("enableClock", po::value<bool>(&enableClock)->default_value(true), "enableClock")
      ("startMidiCC", po::value<int>(&startMidiCC)->default_value(13), "startMidiCC")
      ("stopMidiCC", po::value<int>(&stopMidiCC)->default_value(14), "stopMidiCC")
      ("ignoreProgramChanges", po::value<bool>(&ignoreProgramChanges)->default_value(false), "ignoreProgramChanges")
      ("initialBpm", po::value<int>(&initialBpm)->default_value(142), "initialBpm")
      ("tapTempoMinBpm", po::value<int>(&tapTempoMinBpm)->default_value(80), "tapTempoMinBpm")
      ("tapTempoMaxBpm", po::value<int>(&tapTempoMaxBpm)->default_value(200), "tapTempoMaxBpm")
      ("bpmOffsetForMidiCC", po::value<int>(&bpmOffsetForMidiCC)->default_value(70), "bpmOffsetForMidiCC")
      ("velocityRandomOffset", po::value<int>(&velocityRandomOffset)->default_value(-40), "velocityRandomOffset")
      ("velocityMultiDeviceCtrl", po::value<bool>(&velocityMultiDeviceCtrl)->default_value(true), "velocityMultiDeviceCtrl")
      ("velocityMidiCC", po::value<int>(&velocityMidiCC)->default_value(7), "velocityMidiCC")
      ("tempoMidiCC", po::value<int>(&tempoMidiCC)->default_value(10), "tempoMidiCC")
      ("chordMidiCC", po::value<int>(&chordMidiCC)->default_value(11), "chordMidiCC")
      ("routeMidiCC", po::value<int>(&routeMidiCC)->default_value(12), "routeMidiCC");
    po::variables_map vm;

    ifstream file(CONFIG_FILE);
    po::store(po::parse_config_file(file, desc), vm);
    po::notify(vm);
    file.close();

    clockInterval = 60000000000/(initialBpm*24);
    tapTempoMaxInterval = 60000000000/tapTempoMinBpm;
    tapTempoMinInterval = 60000000000/tapTempoMaxBpm;

    mono[0] = input1mono;
    mono[1] = input2mono;
    mono[2] = input3mono;
    mono[3] = input4mono;

    struct timespec lastClock, now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    randomGenerator = new boost::mt19937(now.tv_nsec);

    midiin1 = new RtMidiIn();
    midiin2 = new RtMidiIn();
    midiin3 = new RtMidiIn();
    midiin4 = new RtMidiIn();
    midiout1 = new RtMidiOut();
	midiout2 = new RtMidiOut();
	midiout3 = new RtMidiOut();
	midiout4 = new RtMidiOut();

    // Assign MIDI ports
    if (!openPorts(input1, input2, input3, input4, output, output2, output3, output4)) {
      cout << "Exiting" << endl;
      cleanUp();
      exit(0);
    }

    // Note off message
    vector<unsigned char> offMsg;
    offMsg.push_back(BOOST_BINARY(10000000));
    offMsg.push_back(42);
    offMsg.push_back(100);
    noteOffMessage = &offMsg;

    // Clock messages
    vector<unsigned char> clkMsg;
    clkMsg.push_back(BOOST_BINARY(11111000));
    clockMessage = &clkMsg;

    // Midi clock start
    vector<unsigned char> clkStartMsg;
    clkStartMsg.push_back(BOOST_BINARY(11111010));
    clockStartMessage = &clkStartMsg;

    // Midi clock stop
    vector<unsigned char> clkStopMsg;
    clkStopMsg.push_back(BOOST_BINARY(11111100));
    clockStopMessage = &clkStopMsg;

    // Tap-tempo
    boost::circular_buffer<struct timespec> taps(4);
    taps.push_front(now);
    tapTempoTimes = &taps;

    map<int, RtMidiIn*> midiins;
    if (midiin1->isPortOpen()) midiins[0] = midiin1;
    if (midiin2->isPortOpen()) midiins[1] = midiin2;
    if (midiin3->isPortOpen()) midiins[2] = midiin3;
    if (midiin4->isPortOpen()) midiins[3] = midiin4;

    done = false;
    resetClock = false;
    (void) signal(SIGINT, finish);
    std::vector<unsigned char> incomingMsg;

    cout << "Starting" << endl;
    if (midiout1->isPortOpen()) midiout1->sendMessage(clockMessage);
	if (midiout2->isPortOpen()) midiout2->sendMessage(clockMessage);
	if (midiout3->isPortOpen()) midiout3->sendMessage(clockMessage);
	if (midiout4->isPortOpen()) midiout4->sendMessage(clockMessage);
    clock_gettime(CLOCK_MONOTONIC, &lastClock);

    while (!done) {
      for(map<int, RtMidiIn*>::iterator iter = midiins.begin(); iter != midiins.end(); ++iter) {
        iter->second->getMessage(&incomingMsg);
        if (incomingMsg.size() > 0) handleMessage(&incomingMsg, iter->first);
      }
      clock_gettime(CLOCK_MONOTONIC, &now);
      if(resetClock || ((now.tv_nsec-lastClock.tv_nsec)+((now.tv_sec-lastClock.tv_sec)*1000000000)) >= clockInterval) {
		if (midiout1->isPortOpen()) midiout1->sendMessage(clockMessage);
		if (midiout2->isPortOpen()) midiout2->sendMessage(clockMessage);
		if (midiout3->isPortOpen()) midiout3->sendMessage(clockMessage);
		if (midiout4->isPortOpen()) midiout4->sendMessage(clockMessage);
        clock_gettime(CLOCK_MONOTONIC, &lastClock);
        resetClock = false;
      }
      usleep (sleepUSec);
    }
    cout << endl;
  }
  catch (RtMidiError &error) {
    error.printMessage();
  }
  catch (exception& e) {
    cout << "Error occurred while reading configuration: " << e.what() << endl;
    return 1;
  }

  cleanUp();
  return 0;
}

void usage(void) {
  cout << "Usage: ./midicloro [-c to start interactive configuration]" << endl;
  exit(0);
}

double random01() {
  static boost::uniform_01<boost::mt19937> dist(*randomGenerator);
  return dist();
}

bool ignoreMessage(unsigned char msgByte) {
  if ((enableClock && (msgByte == BOOST_BINARY(11111000))) || // MIDI clock
      (ignoreProgramChanges && ((msgByte & BOOST_BINARY(11110000)) == BOOST_BINARY(11000000)))) // Program change
    return true;
  return false;
}

void transposeAndSend(vector<unsigned char> *message, int semiNotes) {
  // Verify that the note will end up withing the permitted range
  int note = (int)(*message)[1] + semiNotes;
  if (note >= 0 && note <= 127){
    // This changes the message - keep in mind for the next note in the chord
    (*message)[1] = note;
    midiout1->sendMessage(message);
  }
}

void sendNoteOrChord(vector<unsigned char> *message, int source) {
  int channel = (int)((*message)[0] & BOOST_BINARY(00001111));
  // Handle chord mode
  switch(chordModes[source][channel]) {
    case CHORD_OFF:
      midiout1->sendMessage(message);
      break;
    case MINOR3:
      midiout1->sendMessage(message);
      transposeAndSend(message, 3);
      transposeAndSend(message, 4);
      break;
    case MAJOR3:
      midiout1->sendMessage(message);
      transposeAndSend(message, 4);
      transposeAndSend(message, 3);
      break;
    case MINOR3_LO:
      transposeAndSend(message, -5);
      transposeAndSend(message, 5);
      transposeAndSend(message, 3);
      break;
    case MAJOR3_LO:
      transposeAndSend(message, -5);
      transposeAndSend(message, 5);
      transposeAndSend(message, 4);
      break;
    case MINOR2:
      midiout1->sendMessage(message);
      transposeAndSend(message, 3);
      break;
    case MAJOR2:
      midiout1->sendMessage(message);
      transposeAndSend(message, 4);
      break;
    case M7:
      midiout1->sendMessage(message);
      transposeAndSend(message, 3);
      transposeAndSend(message, 4);
      transposeAndSend(message, 3);
      break;
    case MAJ7:
      midiout1->sendMessage(message);
      transposeAndSend(message, 4);
      transposeAndSend(message, 3);
      transposeAndSend(message, 4);
      break;
    case M9:
      midiout1->sendMessage(message);
      transposeAndSend(message, 3);
      transposeAndSend(message, 4);
      transposeAndSend(message, 3);
      transposeAndSend(message, 4);
      break;
    case MAJ9:
      midiout1->sendMessage(message);
      transposeAndSend(message, 4);
      transposeAndSend(message, 3);
      transposeAndSend(message, 4);
      transposeAndSend(message, 3);
      break;
    case SUS4:
      midiout1->sendMessage(message);
      transposeAndSend(message, 5);
      transposeAndSend(message, 2);
      break;
    case POWER2:
      midiout1->sendMessage(message);
      transposeAndSend(message, 7);
      break;
    case POWER3:
      midiout1->sendMessage(message);
      transposeAndSend(message, 7);
      transposeAndSend(message, 5);
      break;
    case OCTAVE2:
      midiout1->sendMessage(message);
      transposeAndSend(message, 12);
      break;
    case OCTAVE3:
      midiout1->sendMessage(message);
      transposeAndSend(message, 12);
      transposeAndSend(message, 12);
      break;
    default:
      midiout1->sendMessage(message);
      break;
  }
}

void sendNoteOffAndNote(vector<unsigned char> *message, int source) {
  int channel = (int)((*message)[0] & BOOST_BINARY(00001111));
  bool thisIsNoteOn = ((*message)[0] & BOOST_BINARY(10010000)) == BOOST_BINARY(10010000);
  if (!monoLegato[source][channel]) {
    if (thisIsNoteOn && lastNote[source][channel] != -1) {
      (*noteOffMessage)[0] = 128 + channel;
      (*noteOffMessage)[1] = lastNote[source][channel];
      sendNoteOrChord(noteOffMessage, source);
    }
    lastNote[source][channel] = thisIsNoteOn ? (*message)[1] : -1;
    sendNoteOrChord(message, source);
  }
  else {
    unsigned char currNote = (*message)[1];
    sendNoteOrChord(message, source);
    if (thisIsNoteOn && lastNote[source][channel] != -1) {
      (*noteOffMessage)[0] = 128 + channel;
      (*noteOffMessage)[1] = lastNote[source][channel];
      sendNoteOrChord(noteOffMessage, source);
    }
    lastNote[source][channel] = thisIsNoteOn ? currNote : -1;
  }
}


void setChordMode(int source, int channel, int value) {
  if (chordModes[source][channel] == 0 && value == 0)
    monoLegato[source][channel] = !monoLegato[source][channel];

  chordModes[source][channel] = value/8;
}

void routeChannel(vector<unsigned char> *message, int source) {
  int channel = (int)((*message)[0] & BOOST_BINARY(00001111));
  (*message)[0] = ((*message)[0] & BOOST_BINARY(11110000)) + channelRouting[source][channel];
}

void setChannelRouting(int source, int channel, int newChannel) {
  if (newChannel >= 0 && newChannel <= 127)
    channelRouting[source][channel] = newChannel/8;
}

void applyVelocity(vector<unsigned char> *message, int source) {
  int channel = (int)((*message)[0] & BOOST_BINARY(00001111));
  if (velocityModes[source][channel] == VEL_OFF || message->size() < 3)
    return;

  if (velocityModes[source][channel] == VEL_RDM) {
    if (velocityRandomOffset < 0)
      (*message)[2] = max(velocity[source][channel]+(int)(velocityRandomOffset*random01()), 1);
    else if (velocityRandomOffset > 0)
      (*message)[2] = min(velocity[source][channel]+(int)(velocityRandomOffset*random01()), 126) + 1;
    else
      (*message)[2] = max((int)(random01()*127), 1);
  }
  else {
    (*message)[2] = max(velocity[source][channel], 1);
  }
}

void setVelocityMode(int source, int channel, int value) {
  if (value == 127) {
    velocityModes[source][channel] = (velocityModes[source][channel] == VEL_RDM) ? VEL_ON : VEL_RDM;
  }
  else if (value == 0) {
    velocityModes[source][channel] = VEL_OFF;
  }
  else {
    value = scaleUp(value);
    velocity[source][channel] = value;
    if (velocityModes[source][channel] == VEL_OFF)
      velocityModes[source][channel] = VEL_ON;
  }
}

void setVelocityModeMulti(int source, int channel, int value) {
  if (value == 127) {
    int newMode = (velocityModes[source][channel] == VEL_RDM) ? VEL_ON : VEL_RDM;
    for (int i=source; i>=0; i--)
      velocityModes[i][channel] = newMode;
  }
  else if (value == 0) {
    for (int i=source; i>=0; i--)
      velocityModes[i][channel] = VEL_OFF;
  }
  else {
    value = scaleUp(value);
    for (int i=source; i>=0; i--)
      velocity[i][channel] = value;
    if (velocityModes[source][channel] == VEL_OFF)
      for (int i=source; i>=0; i--)
        velocityModes[i][channel] = VEL_ON;
  }
}

int scaleUp(int value) {
  // Scale value to let 8-120 contain the whole range 0-127
  if (value > 64) {
    value += 8*(value - 64)/56;
    value = min(value, 127);
  }
  else if (value < 64) {
    value -= 8*(64 - value)/56;
    value = max(value, 0);
  }
  return value;
}

long tapTempo() {
  long diff = 0;
  long accumulatedDiffs = 0;
  unsigned int i = 0;
  struct timespec now, curr, prev;
  clock_gettime(CLOCK_MONOTONIC, &now);
  tapTempoTimes->push_front(now);
  do {
    curr = (*tapTempoTimes)[i];
    prev = (*tapTempoTimes)[i+1];
    diff = (curr.tv_nsec - prev.tv_nsec) + (curr.tv_sec - prev.tv_sec) * 1000000000;
    accumulatedDiffs += diff;
    i++;
  }
  while (diff >= tapTempoMinInterval && diff <= tapTempoMaxInterval && i < tapTempoTimes->size()-1);
  if (i > 1)
    return accumulatedDiffs/i; // Interval in ns
  else
    return 0;
}

void handleMessage(vector<unsigned char> *message, int source) {
  // Handle mono mode
  if (mono[source] && ((*message)[0] & BOOST_BINARY(11100000)) == BOOST_BINARY(10000000)) {
    routeChannel(message, source);
    applyVelocity(message, source);
    sendNoteOffAndNote(message, source);
  }
  // Note on/off: send note or chord
  else if (((*message)[0] & BOOST_BINARY(11100000)) == BOOST_BINARY(10000000)) {
    routeChannel(message, source);
    applyVelocity(message, source);
    sendNoteOrChord(message, source);
  }
  // Start message: pass it through and reset clock
  else if (enableClock && ((*message)[0] == BOOST_BINARY(11111010))) {
    if (midiout1->isPortOpen()) midiout1->sendMessage(message);
    if (midiout2->isPortOpen()) midiout2->sendMessage(message);
    if (midiout3->isPortOpen()) midiout3->sendMessage(message);
    if (midiout4->isPortOpen()) midiout4->sendMessage(message);
    resetClock = true;
  }
  // Stop message: reset last notes
  else if (enableClock && ((*message)[0] == BOOST_BINARY(11111100))) {
    if (midiout1->isPortOpen()) midiout1->sendMessage(message);
    if (midiout2->isPortOpen()) midiout2->sendMessage(message);
    if (midiout3->isPortOpen()) midiout3->sendMessage(message);
    if (midiout4->isPortOpen()) midiout4->sendMessage(message);
    for (int i=0; i<4; i++)
      for (int j=0; j<16; j++)
        lastNote[i][j] = -1;
  }
  // Tap-tempo MIDI CC: use tap-tempo or tempo from MIDI message
  else if (((*message)[0] & BOOST_BINARY(11110000)) == BOOST_BINARY(10110000) && message->size() > 2 && (*message)[1] == tempoMidiCC) {
    long tapInterval = tapTempo();
    if (tapInterval != 0)
      clockInterval = tapInterval/24;
    else
      clockInterval = 60000000000/((bpmOffsetForMidiCC+(*message)[2])*24);

    resetClock = true;
  }
  // Chord mode MIDI CC: set chord mode
  else if (((*message)[0] & BOOST_BINARY(11110000)) == BOOST_BINARY(10110000) && message->size() > 2 && (*message)[1] == chordMidiCC) {
    routeChannel(message, source);
    setChordMode(source, (*message)[0] & BOOST_BINARY(00001111), (*message)[2]);
  }
  // Channel routing MIDI CC: set channel routing
  else if (((*message)[0] & BOOST_BINARY(11110000)) == BOOST_BINARY(10110000) && message->size() > 2 && (*message)[1] == routeMidiCC) {
    setChannelRouting(source, (*message)[0] & BOOST_BINARY(00001111), (*message)[2]);
  }
  // Velocity MIDI CC: set velocity mode
  else if (((*message)[0] & BOOST_BINARY(11110000)) == BOOST_BINARY(10110000) && message->size() > 2 && (*message)[1] == velocityMidiCC) {
    routeChannel(message, source);
    if (velocityMultiDeviceCtrl)
      setVelocityModeMulti(source, (*message)[0] & BOOST_BINARY(00001111), (*message)[2]);
    else
      setVelocityMode(source, (*message)[0] & BOOST_BINARY(00001111), (*message)[2]);
  }
  // Start message CC: Send midi clock start
  else if (((*message)[0] & BOOST_BINARY(11110000)) == BOOST_BINARY(10110000) && message->size() > 2 && (*message)[1] == startMidiCC && (*message)[2] >= 64) {
    if (midiout1->isPortOpen()) midiout1->sendMessage(clockStartMessage);
    if (midiout2->isPortOpen()) midiout2->sendMessage(clockStartMessage);
    if (midiout3->isPortOpen()) midiout3->sendMessage(clockStartMessage);
    if (midiout4->isPortOpen()) midiout4->sendMessage(clockStartMessage);
  }
  // Stop message CC: Send midi clock stop
  else if (((*message)[0] & BOOST_BINARY(11110000)) == BOOST_BINARY(10110000) && message->size() > 2 && (*message)[1] == stopMidiCC && (*message)[2] >= 64) {
    if (midiout1->isPortOpen()) midiout1->sendMessage(clockStopMessage);
    if (midiout2->isPortOpen()) midiout2->sendMessage(clockStopMessage);
    if (midiout3->isPortOpen()) midiout3->sendMessage(clockStopMessage);
    if (midiout4->isPortOpen()) midiout4->sendMessage(clockStopMessage);
  }
  // Other MIDI messages
  else if (!ignoreMessage((*message)[0])) {
    if ((((*message)[0] & BOOST_BINARY(11110000)) >= BOOST_BINARY(10000000)) &&
        (((*message)[0] & BOOST_BINARY(11110000)) <= BOOST_BINARY(11100000))) {
      routeChannel(message, source);
    }
    if (midiout1->isPortOpen()) midiout1->sendMessage(message);
    if (midiout2->isPortOpen()) midiout2->sendMessage(message);
    if (midiout3->isPortOpen()) midiout3->sendMessage(message);
    if (midiout4->isPortOpen()) midiout4->sendMessage(message);
  }
}

void messageAtIn1(double deltatime, vector<unsigned char> *message, void */*userData*/) {
  handleMessage(message, 0);
}

void messageAtIn2(double deltatime, vector<unsigned char> *message, void */*userData*/) {
  handleMessage(message, 1);
}

void messageAtIn3(double deltatime, vector<unsigned char> *message, void */*userData*/) {
  handleMessage(message, 2);
}

void messageAtIn4(double deltatime, vector<unsigned char> *message, void */*userData*/) {
  handleMessage(message, 3);
}

string trimPort(bool doTrim, const string& str) {
  if (doTrim)
    return str.substr(0,str.find_last_of(" "));
  return str;
}

bool openInputPort(RtMidiIn *in, string port) {
  if (port.empty())
    return false;

  // Match full name if port contains hardware id (example: 11:0), otherwise remove the hardware id before matching
  bool doTrim = !boost::regex_match(port, boost::regex("(.+)\\s([0-9]+):([0-9]+)"));
  string portName;
  unsigned int i = 0, nPorts = in->getPortCount();
  for (i=0; i<nPorts; i++ ) {
    portName = in->getPortName(i);
    if (trimPort(doTrim, portName) == port) {
      cout << "Opening input port: " << portName << endl;
      in->openPort(i);
      in->ignoreTypes(false, false, false);
      return true;
    }
  }
  cout << "Couldn't find input port: " << port << endl;
  return false;
}

bool openOutputPort(RtMidiOut *out, string port) {
	if (port.empty())
    return false;
    
  // Match full name if port contains hardware id (example: 11:0), otherwise remove the hardware id before matching
  bool doTrim = !boost::regex_match(port, boost::regex("(.+)\\s([0-9]+):([0-9]+)"));
  string portName;
  unsigned int i = 0, nPorts = out->getPortCount();
  for (i=0; i<nPorts; i++ ) {
    portName = out->getPortName(i);
    if (trimPort(doTrim, portName) == port) {
      cout << "Opening output port: " << portName << endl;
      out->openPort(i);
      return true;
    }
  }
  cout << "Couldn't find output port: " << port << endl;
  return false;
}

bool openPorts(string i1, string i2, string i3, string i4, string o1, string o2, string o3, string o4) {
  openInputPort(midiin1, i1);
  openInputPort(midiin2, i2);
  openInputPort(midiin3, i3);
  openInputPort(midiin4, i4);
  openOutputPort(midiout2, o2);
  openOutputPort(midiout3, o3);
  openOutputPort(midiout4, o4);
  return openOutputPort(midiout1, o1);
}

void cleanUp() {
  delete midiin1;
  delete midiin2;
  delete midiin3;
  delete midiin4;
  delete midiout1;
  delete midiout2;
  delete midiout3;
  delete midiout4;
}

void runInteractiveConfiguration() {
  cout << "This will clear and reconfigure the settings. Continue? (y/N): ";
  string keyHit;
  getline(cin, keyHit);
  if (keyHit != "y") {
    cout << "Exiting" << endl;
    exit(0);
  }
  RtMidiIn *cfgMidiIn = new RtMidiIn();
  RtMidiOut *cfgMidiOut = new RtMidiOut();
  string cfg = "";
  vector<string> inputs;
  vector<string> outputs;
  string portName;
  int userIn;
  int addedIns = 0;
  int addedOuts = 0;
  cout << endl <<
    "Note about hardware id (HWid, example: 11:0). "
    "The HWid for a port might change if you connect it to another USB port."
    << endl <<
    "Never store HWid except for ports which have the same name (storing HWid is required in that case)."
    << endl << endl;
  // Input
  int nPorts = cfgMidiIn->getPortCount();
  cout << "Available input ports:" << endl;
  for (int i=0; i<nPorts; i++) {
    portName = cfgMidiIn->getPortName(i);
    inputs.push_back(portName);
    cout << i << ". " << portName << endl;
  }
  for (int i=0; i<4; i++) {
    cout << "Enter port number for input " << i+1 << " (press enter to disable)" << ": ";
    if (addedIns>=nPorts) {
      cout << endl << "Disabling input" << i+1 << endl;
      cfg += string("input") + convert::to_string(i+1) + string(" =\n");
      continue;
    }
    else if (cin.peek()=='\n' || !(cin >> userIn) || userIn<0 || userIn>=nPorts || inputs[userIn]=="") {
      cout << "Disabling input " << i+1 << endl;
      cfg += string("input") + convert::to_string(i+1) + string(" =\n");
      cin.clear();
      cin.ignore(numeric_limits<streamsize>::max(), '\n');
      continue;
    }
    cin.clear();
    cin.ignore(numeric_limits<streamsize>::max(), '\n');

    cout << "Store hardware id for input " << i+1 << "? (y/N): ";
    getline(cin, keyHit);
    if (keyHit == "y")
      cfg += string("input") + convert::to_string(i+1) + string(" = ") + inputs[userIn] + "\n";
    else
      cfg += string("input") + convert::to_string(i+1) + string(" = ") + trimPort(true, inputs[userIn]) + "\n";

    cout << "Enable mono mode for input " << i+1 << "? (y/N): ";
    getline(cin, keyHit);
    if (keyHit == "y")
      cfg += string("input") + convert::to_string(i+1) + string("mono = true") + "\n";

    addedIns++;
    inputs[userIn] = "";
  }
  
  // Output
  nPorts = cfgMidiOut->getPortCount();
  cout << endl << "Available output ports:" << endl;
  for (int i=0; i<nPorts; i++) {
    portName = cfgMidiOut->getPortName(i);
    outputs.push_back(portName);
    cout << i << ". " << portName << endl;
  }

  for (int i=0; i<4; i++) {
    cout << "Enter port number for output " << i+1 << " (press enter to disable)" << ": ";
    if (addedOuts>=nPorts) {
      cout << endl << "Disabling output" << i+1 << endl;
      cfg += string("output") + convert::to_string(i+1) + string(" =\n");
      continue;
    }
    else if (cin.peek()=='\n' || !(cin >> userIn) || userIn<0 || userIn>=nPorts || outputs[userIn]=="") {
      cout << "Disabling output " << i+1 << endl;
      cfg += string("output") + convert::to_string(i+1) + string(" =\n");
      cin.clear();
      cin.ignore(numeric_limits<streamsize>::max(), '\n');
      continue;
    }
    cin.clear();
    cin.ignore(numeric_limits<streamsize>::max(), '\n');

    cout << "Store hardware id for output " << i+1 << "? (y/N): ";
    getline(cin, keyHit);
    if (keyHit == "y")
      cfg += string("output") + convert::to_string(i+1) + string(" = ") + outputs[userIn] + "\n";
    else
      cfg += string("output") + convert::to_string(i+1) + string(" = ") + trimPort(true, outputs[userIn]) + "\n";

    addedOuts++;
    outputs[userIn] = "";
  }

  cout << "Enter number of microseconds to sleep between loops. Low for better MIDI performance, high for slow machines (Min: 10, Max 10000, default 100): ";
  if (cin.peek()=='\n' || !(cin >> userIn) || userIn<10 || userIn>10000)
    cfg += string("sleepUSec = 100") + "\n";
  else
    cfg += string("sleepUSec = ") + convert::to_string(userIn) + "\n";

  cout << endl << "Enable MIDI clock? (Y/n): ";
  getline(cin, keyHit);
  if (keyHit == "n")
    cfg += string("enableClock = false") + "\n";
  else
    cfg += string("enableClock = true") + "\n";

  cout << "Enter start clock MIDI CC number (default 13): ";
  if (cin.peek()=='\n' || !(cin >> userIn) || userIn<0 || userIn>127)
    cfg += string("startMidiCC = 13") + "\n";
  else
    cfg += string("startMidiCC = ") + convert::to_string(userIn) + "\n";

  cin.clear();
  cin.ignore(numeric_limits<streamsize>::max(), '\n');

  cout << "Enter stop clock MIDI CC number (default 14): ";
  if (cin.peek()=='\n' || !(cin >> userIn) || userIn<0 || userIn>127)
    cfg += string("stopMidiCC = 14") + "\n";
  else
    cfg += string("stopMidiCC = ") + convert::to_string(userIn) + "\n";

  cin.clear();
  cin.ignore(numeric_limits<streamsize>::max(), '\n');

  cout << "Ignore incoming program change messages? (y/N): ";
  getline(cin, keyHit);
  if (keyHit == "y")
    cfg += string("ignoreProgramChanges = true") + "\n";
  else
    cfg += string("ignoreProgramChanges = false") + "\n";

  cout << "Enter initial MIDI clock BPM (1-300, default 142): ";
  if (cin.peek()=='\n' || !(cin >> userIn) || userIn<1 || userIn>300)
    cfg += string("initialBpm = 142") + "\n";
  else
    cfg += string("initialBpm = ") + convert::to_string(userIn) + "\n";

  cin.clear();
  cin.ignore(numeric_limits<streamsize>::max(), '\n');

  cout << "Enter tap-tempo minimum BPM (default 80): ";
  if (cin.peek()=='\n' || !(cin >> userIn) || userIn<0)
    cfg += string("tapTempoMinBpm = 80") + "\n";
  else
    cfg += string("tapTempoMinBpm = ") + convert::to_string(userIn) + "\n";

  cin.clear();
  cin.ignore(numeric_limits<streamsize>::max(), '\n');

  cout << "Enter tap-tempo maximum BPM (default 200): ";
  if (cin.peek()=='\n' || !(cin >> userIn) || userIn<0)
    cfg += string("tapTempoMaxBpm = 200") + "\n";
  else
    cfg += string("tapTempoMaxBpm = ") + convert::to_string(userIn) + "\n";

  cin.clear();
  cin.ignore(numeric_limits<streamsize>::max(), '\n');

  cout << "Enter BPM offset (offset + MIDI CC value [0-127] = BPM, default 70): ";
  if (cin.peek()=='\n' || !(cin >> userIn) || userIn<0)
    cfg += string("bpmOffsetForMidiCC = 70") + "\n";
  else
    cfg += string("bpmOffsetForMidiCC = ") + convert::to_string(userIn) + "\n";

  cin.clear();
  cin.ignore(numeric_limits<streamsize>::max(), '\n');

  cout << "Enter velocity random offset (default -40): ";
  if (cin.peek()=='\n' || !(cin >> userIn) || userIn < -127 || userIn > 127)
    cfg += string("velocityRandomOffset = -40") + "\n";
  else
    cfg += string("velocityRandomOffset = ") + convert::to_string(userIn) + "\n";

  cin.clear();
  cin.ignore(numeric_limits<streamsize>::max(), '\n');

  cout << "Enable velocity multi device control (e.g. input 3 controls the velocity setting for input 3, 2 and 1)? (Y/n): ";
  getline(cin, keyHit);
  if (keyHit == "n")
    cfg += string("velocityMultiDeviceCtrl = false") + "\n";
  else
    cfg += string("velocityMultiDeviceCtrl = true") + "\n";

  cout << "Enter velocity MIDI CC number (default 7): ";
  if (cin.peek()=='\n' || !(cin >> userIn) || userIn<0 || userIn>127)
    cfg += string("velocityMidiCC = 7") + "\n";
  else
    cfg += string("velocityMidiCC = ") + convert::to_string(userIn) + "\n";

  cin.clear();
  cin.ignore(numeric_limits<streamsize>::max(), '\n');

  cout << "Enter tempo MIDI CC number (default 10): ";
  if (cin.peek()=='\n' || !(cin >> userIn) || userIn<0 || userIn>127)
    cfg += string("tempoMidiCC = 10") + "\n";
  else
    cfg += string("tempoMidiCC = ") + convert::to_string(userIn) + "\n";

  cin.clear();
  cin.ignore(numeric_limits<streamsize>::max(), '\n');

  cout << "Enter chord mode MIDI CC number (default 11): ";
  if (cin.peek()=='\n' || !(cin >> userIn) || userIn<0 || userIn>127)
    cfg += string("chordMidiCC = 11") + "\n";
  else
    cfg += string("chordMidiCC = ") + convert::to_string(userIn) + "\n";

  cin.clear();
  cin.ignore(numeric_limits<streamsize>::max(), '\n');

  cout << "Enter channel routing MIDI CC number (default 12): ";
  if (cin.peek()=='\n' || !(cin >> userIn) || userIn<0 || userIn>127)
    cfg += string("routeMidiCC = 12") + "\n";
  else
    cfg += string("routeMidiCC = ") + convert::to_string(userIn) + "\n";

  cin.clear();
  cin.ignore(numeric_limits<streamsize>::max(), '\n');

  cout << endl;

  ofstream file(CONFIG_FILE);
  file << cfg;

  delete cfgMidiIn;
  delete cfgMidiOut;
}
