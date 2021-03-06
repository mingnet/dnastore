#ifndef DECODER_INCLUDED
#define DECODER_INCLUDED

#include "trans.h"
#include "logger.h"

template<class Writer>
struct Decoder {
  typedef map<State,deque<InputSymbol> > StateString;
  typedef typename StateString::iterator StateStringIter;
  
  const Machine& machine;
  Writer& outs;
  StateString current;

  Decoder (const Machine& machine, Writer& outs)
    : machine(machine),
      outs(outs)
  {
    current[machine.startState()] = deque<InputSymbol>();
    expand();
  }

  ~Decoder() {
    close();
  }

  void close() {
    if (current.size()) {
      expand();
      vguard<StateStringIter> ssIter;
      for (StateStringIter ss = current.begin(); ss != current.end(); ++ss)
	if (machine.state[ss->first].isEnd())
	  ssIter.push_back (ss);
      if (ssIter.size() == 1)
	flush (ssIter.front());
      else if (ssIter.size() > 1) {
	Warn ("Decoder unresolved: %u possible end states", ssIter.size());
	for (auto ss: ssIter)
	  Warn ("State %s: input queue %s", machine.state[ss->first].name.c_str(), ss->second.empty() ? "empty" : to_string_join(ss->second,"").c_str());
      } else if (current.size() > 1) {
	Warn ("Decoder unresolved: %u possible states", current.size());
	showQueue();
      }
      current.clear();
    }
  }

  void showQueue() const {
    for (const auto& ss: current)
      Warn ("State %s: input queue %s", machine.state[ss.first].name.c_str(), ss.second.empty() ? "empty" : to_string_join(ss.second,"").c_str());
  }
  
  void expand() {
    StateString next, seen;
    bool foundNew;
    do {
      foundNew = false;
      for (const auto& ss: current) {
	seen.insert (ss);
	const State state = ss.first;
	const auto& str = ss.second;
	const MachineState& ms = machine.state[state];
	LogThisAt(10,"Input queue for " << ms.name << " is " << (str.empty() ? string("empty") : string(str.begin(),str.end())) << endl);
	if (ms.isEnd() || ms.emitsOutput())
	  next[state] = str;
      }
      for (const auto& ss: current) {
	const State state = ss.first;
	const auto& str = ss.second;
	const MachineState& ms = machine.state[state];
	for (const auto& t: ms.trans)
	  if (isUsable(t) && t.outputEmpty()) {
	    auto nextStr = str;
	    if (!t.inputEmpty())
	      nextStr.push_back (t.in);
	    if (seen.count (t.dest))
	      Assert (seen.at(t.dest) == nextStr,
		      "Decoder error: state %s has two possible input queues (%s, %s)",
		      machine.state[t.dest].name.c_str(),
		      to_string_join(seen.at(t.dest),"").c_str(),
		      to_string_join(nextStr,"").c_str());
	    else {
	      next[t.dest] = nextStr;
	      LogThisAt(9,"Transition " << ms.name
			<< " -> " << machine.state[t.dest].name
			<< (nextStr.empty() ? string() : (string(": input queue ") + to_string_join(nextStr,"")))
			<< endl);
	      foundNew = true;
	    }
	  }
      }
      current.swap (next);
      next.clear();
    } while (foundNew);
  }

  void write (const char* s, size_t len) {
    char* buf = (char*) malloc (sizeof(char) * (len + 1));
    for (size_t n = 0; n < len; ++n)
      buf[n] = s[n];
    buf[len] = '\0';
    (void) outs.write (buf, len);
    free (buf);
  }
  
  void flush (StateStringIter ss) {
    const string str (ss->second.begin(), ss->second.end());
    if (str.size()) {
      LogThisAt(9,"Flushing input queue: " << str << endl);
      write (str.c_str(), str.size());
      ss->second.clear();
    }
  }

  static bool isUsable (const MachineTransition& t) {
    return t.in == MachineNull
      || t.in == MachineBit0 || t.in == MachineBit1
      || t.in == MachineEOF || t.in == MachineSOF
      || Machine::isControl(t.in);
  }
  
  void decodeSymbol (OutputSymbol outSym) {
    LogThisAt(8,"Decoding " << outSym << endl);
    StateString next;
    for (const auto& ss: current) {
      const State state = ss.first;
      const auto& str = ss.second;
      for (const auto& t: machine.state[state].trans)
	if (isUsable(t) && t.out == outSym) {
	  const State nextState = t.dest;
	  auto nextStr = str;
	  if (!t.inputEmpty())
	    nextStr.push_back (t.in);
	  Assert (!next.count(nextState) || next.at(nextState) == nextStr,
		  "Decoder error: state %s has two possible input queues (%s, %s)",
		  machine.state[nextState].name.c_str(),
		  to_string_join(next.at(nextState),"").c_str(),
		  to_string_join(nextStr,"").c_str());
	  next[nextState] = nextStr;
	  LogThisAt(9,"Transition " << machine.state[state].name
		    << " -> " << machine.state[nextState].name
		    << ": "
		    << (nextStr.empty() ? string() : (string("input queue ") + to_string_join(nextStr,"") + ", "))
		    << "output " << t.out
		    << endl);
	}
    }
    Assert (!next.empty(), "Can't decode '%c'", outSym);
    current.swap (next);
    expand();
    if (current.size() == 1) {
      auto iter = current.begin();
      const MachineState& ms = machine.state[iter->first];
      if (ms.exitsWithInput())
	flush (iter);
    } else
      shiftResolvedSymbols();
  }

  void shiftResolvedSymbols() {
    while (true) {
      bool foundQueue = false, queueNonempty, firstCharSame;
      InputSymbol firstChar;
      for (const auto& ss: current) {
	if (!foundQueue) {
	  if ((queueNonempty = !ss.second.empty()))
	    firstChar = ss.second[0];
	  foundQueue = firstCharSame = true;
	} else if (queueNonempty
		   && (ss.second.empty()
		       || firstChar != ss.second[0])) {
	  firstCharSame = false;
	  break;
	}
      }
      if (foundQueue && queueNonempty && firstCharSame) {
	LogThisAt(9,"All input queues have '" << Machine::charToString(firstChar) << "' as first symbol; shifting" << endl);
	write (&firstChar, 1);
	for (auto& ss: current)
	  ss.second.erase (ss.second.begin());
      } else
	break;
    }
  }

  void decodeString (const string& seq) {
    for (char c: seq)
      decodeSymbol (toupper (c));
  }
};

struct BinaryWriter {
  ostream& outs;
  bool msb0;
  vguard<bool> outbuf;

  BinaryWriter (ostream& outs)
    : outs(outs),
      msb0(false)
  { }

  ~BinaryWriter() {
    if (!outbuf.empty()) {
      if (!msb0)
	reverse (outbuf.begin(), outbuf.end());
      Warn ("%s (%s) remaining on output", plural(outbuf.size(),"bit").c_str(), to_string_join(outbuf,"").c_str());
    }
  }

  void flush() {
    unsigned char c = 0;
    for (size_t n = 0; n < outbuf.size(); ++n)
      if (outbuf[n])
	c = c | (1 << (msb0 ? (7-n) : n));
    LogThisAt(7,"Decoding '" << (char)c << "' (\\x" << hex << (int)c << ")" << endl);
    outs << c;
    outbuf.clear();
  }
  
  void write (char* buf, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      const char c = buf[i];
      if (c == MachineBit0 || c == MachineBit1) {
	outbuf.push_back (c == MachineBit1);
	if (outbuf.size() == 8)
	  flush();
      } else {
	if (Machine::isControl(c))
	  Warn("Ignoring control character #%d ('%c') in decoder",Machine::controlIndex(c),c);
	else if (c == MachineSOF)
	  LogThisAt(2,"Ignoring start-of-file character '" << c << "' in decoder" << endl);
	else if (c == MachineEOF)
	  LogThisAt(2,"Ignoring end-of-file character '" << c << "' in decoder" << endl);
	else
	  Warn("Ignoring unknown character '%c' (\\x%.2x) in decoder",c,c);
      }
    }
  }
};

#endif /* DECODER_INCLUDED */

