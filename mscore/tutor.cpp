//=============================================================================
//  MuseScore
//  Linux Music Score Editor
//
//  Copyright (C) 2002-2011 Werner Schweer and others
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//=============================================================================

#include "config.h"
#include "musescore.h"
#include "tutor.h"

#include <stdio.h>
#include <time.h>
#include <assert.h>
#include <functional>

#include <QTimer>

#ifdef WIN32
#include <windows.h>
#define INVALID_SERIAL ((int) INVALID_HANDLE_VALUE)
#else
#include <termios.h>
#define INVALID_SERIAL -1
#endif

#ifdef WIN32
static const char *DEFAULT_SERIAL_DEVICE="COM0";
#else
static const char *DEFAULT_SERIAL_DEVICE="/dev/ttyACM0";
#endif

int def_colors[3][3] = {
  { 16, 0, 0},  // default color for mistakes
  { 16, 0, 16}, // default color for even channels
  { 0, 16, 16}  // default color for odd channels
};

/* Cross-OS Win32 / Linux functions - BEGIN */

static void MILLI_SLEEP(unsigned int ms) {
#ifdef WIN32
  Sleep(ms);
#else
  usleep(ms * 1000);
#endif
}

void CLOSE(int fd) {
#ifdef WIN32
  CloseHandle((HANDLE)fd);
#else
  close(fd);
#endif
}

ssize_t WRITE(int fd, void *buf, size_t count) {
#ifdef WIN32
  DWORD len;
  if (WriteFile((HANDLE)fd, buf, count, &len, NULL) == 0) {
    qDebug("WriteFile() failed!");
    return -1;
  }
  return len;
#else
  return write(fd, buf, count);
#endif
}

ssize_t READ(int fd, void *buf, size_t count) {
#ifdef WIN32
  DWORD len;
  if (ReadFile((HANDLE)fd, buf, count, &len, NULL) == 0) {
    qDebug("ReadFile() failed!");
    return -1;
  }
  return len;
#else
  return read(fd, buf, count);
#endif
}

/* Cross-OS functions - END */

Tutor::Tutor() : serialDevice(DEFAULT_SERIAL_DEVICE),
                 tutorSerial(INVALID_SERIAL), num_curr_events(0),
		 c4light(71), coeff(-2.0),
		 needs_flush(false),
                 lit_until_release(false)
{
  // mark all notes as unused
  for (int i = 0; i < 256; i++) {
    notes[i].velo = -1;
  }
  memcpy(colors, def_colors, sizeof(colors));
}

#ifdef WIN32
bool Tutor::checkSerial() {
  if (tutorSerial != INVALID_SERIAL)
    return true;

  tutorSerial = (int) CreateFile(serialDevice.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
  if (tutorSerial == INVALID_SERIAL) {
    qDebug("CreateFile() failed: serialDevice=%s", serialDevice.c_str());
    return false;
  }
  DCB config;
  if (GetCommState((HANDLE)tutorSerial, &config) == 0) {
    qDebug("GetCommState() failed");
    return false;
  }
  config.BaudRate = 115200;
  if (SetCommState((HANDLE)tutorSerial, &config) == 0) {
    qDebug("SetCommState() failed");
    return false;
  }
  // Waiting for "PianoTutor v1.0 is ready!" string
  int to_read = 25;
  while (to_read > 0) {
    char ch;
    int len = READ(tutorSerial, &ch, 1);
    if (len < 0) {
      perror("read() failed: ");
      return false;
    }
    to_read -= len;
  }
  MILLI_SLEEP(10);
  return true;
}
#else
bool Tutor::checkSerial() {
  if (tutorSerial != INVALID_SERIAL)
    return true;

  tutorSerial = open(serialDevice.c_str(), O_RDWR | O_NOCTTY);
  if (tutorSerial == INVALID_SERIAL) {
    qDebug("open() failed: serialDevice=%s", serialDevice.c_str());
    return false;
  }

  termios tio;
  if (tcgetattr(tutorSerial, &tio) < 0) {
    perror("tcgetattr() failed: ");
    return false;
  }
  tio.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
  tio.c_cflag |= CS8 | CREAD | CLOCAL;
  tio.c_iflag &= ~(IXON | IXOFF | IXANY);
  tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
  tio.c_oflag = 0;
  tio.c_cc[VTIME] = 1; /* In 1/10 of a second: set timeout to 0.1s       */
  tio.c_cc[VMIN] = 0;  /* No minimum bytes to wait for, before returning */
  if (cfsetispeed(&tio, B115200) < 0 || cfsetospeed(&tio, B115200) < 0) {
    perror("cfsetXspeed() failed: ");
    return false;
  }
  tcsetattr(tutorSerial, TCSANOW, &tio);
  if (tcsetattr(tutorSerial, TCSAFLUSH, &tio) < 0) {
    perror("tcsetattr() failed: ");
    return false;
  }
  // Waiting for "PianoTutor v1.0 is ready!" string
  int to_read = 25;
  while (to_read > 0) {
    char ch;
    int len = READ(tutorSerial, &ch, 1);
    if (len < 0) {
      perror("read() failed: ");
      return false;
    }
    to_read -= len;
  }
  MILLI_SLEEP(10);
  return true;
}
#endif

void Tutor::safe_write(char *data, int len, bool flush_op) {
  assert(!mtx.try_lock());

  // flush operation(s) update LEDs on the stripe, which is blocking
  // Arduino for a while -- without it pulling bytes out of its
  // hardware 1-byte buffer (!) -- let's have a ping-pong with
  // Arduino before writing anything, to be sure it's listening back.
  // note: spin-waiting with lock held in RT thread (!)

  const char *ping_str = "P\n";
  char pong_char = '.';
  int bytes_read;
  do {
    WRITE(tutorSerial, (void*)ping_str, 2);
    bytes_read = READ(tutorSerial, &pong_char, 1);
    if (pong_char != 'P')
      continue;
  } while (bytes_read != 1);

  // useful to debug/printf() what's about to be written (beware of buffer overruns)
  int retry = 2;
  data[len] = '\0';
  while (len > 0) {
    int written = WRITE(tutorSerial, data, len);
    qDebug("Written %d bytes (len=%d): %s", written, len, data);
    if (written < 0) {
      perror("write() failed!");
      CLOSE(tutorSerial);
      tutorSerial = INVALID_SERIAL;
      if (retry-- > 0) {
	checkSerial();
	continue;
      }
      return;
    }
    data += written;
    len -= written;
  }
}

void Tutor::flushNoLock() {
  assert(!mtx.try_lock());
  if (checkSerial() && needs_flush) {
    char cmd[4];
    cmd[0]='F';
    cmd[1]='\n';
    qDebug("flushNoLock(): calling safe_write()");
    safe_write(cmd, 2, true);
    needs_flush = false;
  }
}

int Tutor::pitchToLight(int pitch) {
  int reflight, diffpitch;
  if (pitch >= 60) {
    reflight = c4light;
    diffpitch = pitch - 60;
  } else {
    reflight = c4light + (coeff > 0 ? -1 : 1);
    diffpitch = pitch - 59;
  }
  int led = round(diffpitch * coeff + reflight);
  if (led < 0)
    led = 0;
  else if (led > 255)
    led = 255;
  //printf("pitch %d -> light %d\n", pitch, led);
  return led;
}

void Tutor::tuneC4Pitch(int pitch) {
  clearKeys();
  c4light -= round((pitch - 60) * coeff);
}

void Tutor::setTutorLight(int pitch, int velo, int channel, int future) {
  assert(!mtx.try_lock());
  if (checkSerial()) {
    int r, g, b;
    if (channel == -1) {
      r = colors[0][0];
      g = colors[0][1];
      b = colors[0][2];
    } else {
      r = colors[1 + channel % 2][0];
      g = colors[1 + channel % 2][1];
      b = colors[1 + channel % 2][2];
    }
    if (future > 0) {
      r /= 8;
      g /= 8;
      b /= 8;
    }
    char cmd[16];
    int cmdlen = snprintf(cmd, sizeof(cmd) - 1,
			  "H%02x%02x%02x%02x\n", pitchToLight(pitch), r, g, b);
    safe_write(cmd, cmdlen, false);
    needs_flush = true;
  }
}

void Tutor::clearTutorLight(int pitch) {
  assert(!mtx.try_lock());
  if (checkSerial()) {
    char cmd[16];
    int cmdlen = snprintf(cmd, sizeof(cmd) - 1,
			  "H%02x%02x%02x%02x\n", pitchToLight(pitch), 0, 0, 0);
    safe_write(cmd, cmdlen, false);
    needs_flush = true;
  }
}

// Mark a tutor light as pressed (dark grey), waiting for the note_off event to actually clear it
void Tutor::setTutorLightPressed(int pitch) {
  assert(!mtx.try_lock());
  if (checkSerial()) {
    int r = 2;
    int g = 2;
    int b = 2;
    char cmd[16];
    int cmdlen = snprintf(cmd, sizeof(cmd) - 1,
			  "H%02x%02x%02x%02x\n", pitchToLight(pitch), r, g, b);
    safe_write(cmd, cmdlen, false);
    needs_flush = true;
  }
}

void Tutor::addKey(int pitch, int velo, int channel, int future) {
  struct timespec prev = (struct timespec) {0, 0};
  if (velo == 0) {
    clearKey(pitch);
    return;
  }
  pitch &= 255;
  tnote & n = notes[pitch];
  std::lock_guard<std::mutex> lock(mtx);
  if (velo == n.velo && channel == n.channel && future == n.future)
    return;
  qDebug("addKey(): p=%d, v=%d, c=%d, f=%d", pitch, velo, channel, future);
  if (channel == -1) {
    setTutorLight(pitch, velo, channel, 0);
    n.velo = -2;
    return;
  }
  if (n.velo != -1 && n.velo != -2) {
    if (future == 0 && n.future > 0) {
      ++num_curr_events;
      if (n.ts.tv_sec != 0 || n.ts.tv_nsec != 0)
	prev = n.ts;
    } else if (future > n.future || (future == n.future && velo < n.velo)) {
      return;
    }
  } else {
    if (future == 0)
      ++num_curr_events;
  }
  n = (tnote) {velo, channel, future, {0, 0}};
  setTutorLight(pitch, velo, channel, future);

  // Future keys pressed less than 100ms ago are automatically cleared
  // Purposedly light up then turn off LED (unless too fast to be visible)
  if (prev.tv_sec != 0 || prev.tv_nsec != 0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    unsigned long diff_us =
      (now.tv_sec - prev.tv_sec) * 1000000 + (now.tv_nsec - prev.tv_nsec) / 1000;
    if (diff_us < 100000) {
      //printf("diff_us: %lu, now: %ld,%ld, prev: %ld,%ld\n", diff_us, now.tv_sec,now.tv_nsec, prev.tv_sec,prev.tv_nsec);
      clearTutorLight(pitch);
      --num_curr_events;
      n.velo = -1;
    }
    //printf("pitch %d, diff_us: %lu, size: %d\n", pitch, diff_us, num_curr_events);
  }
}

void Tutor::clearKeyNoLock(int pitch, bool mark) {
  qDebug("clearKey(): p=%d", pitch);
  assert(!mtx.try_lock());
  pitch &= 255;
  tnote & n = notes[pitch];
  if (n.velo == -2) {
    clearTutorLight(pitch);
    // num_curr_events was already decreased in this case
    n.velo = -1;
  } else if (n.velo != -1) {
    if (n.future == 0) {
      clearTutorLight(pitch);
      --num_curr_events;
      n.velo = -1;
    } else if (mark) {
      clock_gettime(CLOCK_MONOTONIC, &n.ts);
      //printf("Marking time for pitch %d: %d,%d\n", pitch, n.ts.tv_sec,n.ts.tv_nsec);
    }
  }
}

void Tutor::clearKey(int pitch, bool mark) {
  std::lock_guard<std::mutex> lock(mtx);
  clearKeyNoLock(pitch, mark);
}

void Tutor::clearKeys(int channel) {
  do {
    std::lock_guard<std::mutex> lock(mtx);
    if (channel == -1) {
      if (checkSerial()) {
        char cmd[4];
        cmd[0]='c'; // 'c' also flushes
        cmd[1]='\n';
        safe_write(cmd, 2, true);
        needs_flush = false;
      }
      for (int i = 0; i < (int) (sizeof(notes) / sizeof(notes[0])); ++i)
        notes[i].velo = -1;
      num_curr_events = 0;
    } else {
      for (int i = 0; i < (int) (sizeof(notes) / sizeof(notes[0])); ++i)
        if (notes[i].velo != -1 && notes[i].channel == channel) {
          clearTutorLight(i);
          notes[i].velo = -1;
        }
      flushNoLock();
    }
  } while (false);
}

void Tutor::flush() {
  do {
    std::lock_guard<std::mutex> lock(mtx);
    flushNoLock();
  } while (false);
}

void Tutor::setColor(int idx, int r, int g, int b) {
  colors[idx][0] = r;
  colors[idx][1] = g;
  colors[idx][2] = b;
}

// ms to wait before flushing on keyPressed()
static int FLUSH_TOUT=5;

int Tutor::keyPressed(int pitch, int velo) {
  if (velo == 0)
    return -1;
  pitch &= 255;
  tnote & n = notes[pitch];
  int rv = -1;
  do {
    std::lock_guard<std::mutex> lock(mtx);
    if (n.velo == -1 || n.velo == -2) {
      rv = -1;
      break;
    }
    if (n.future == 0) {
      if (lit_until_release) {
        qDebug("Marking key as pressed: pitch=%d", pitch);
        n.velo = -2;
        setTutorLightPressed(pitch);
      } else {
        qDebug("Clearing key: pitch=%d", pitch);
        n.velo = -1;
        clearTutorLight(pitch);
      }
      --num_curr_events;
      QTimer::singleShot(FLUSH_TOUT, std::bind(std::mem_fn(&Tutor::onFlushTimer), this));
      //flushNoLock();
      rv = 0;
      break;
    } else if (n.future > 0 && num_curr_events == 0) {
      rv = n.future;
      qDebug("Clearing future event & skipping: pitch=%d", pitch);
      clearKeyNoLock(pitch, true);
      QTimer::singleShot(FLUSH_TOUT, std::bind(std::mem_fn(&Tutor::onFlushTimer), this));
      //flushNoLock();
      break;
    }
  } while (false);
  return rv;
}

size_t Tutor::size() {
  std::lock_guard<std::mutex> lock(mtx);
  return num_curr_events;
}

void Tutor::setSerialDevice(const std::string & s) {
  if (tutorSerial != INVALID_SERIAL)
    CLOSE(tutorSerial);
  tutorSerial = INVALID_SERIAL;
  serialDevice = s;
}

std::string Tutor::getSerialDevice() const {
  return serialDevice;
}

void Tutor::onFlushTimer() {
  std::lock_guard<std::mutex> lock(mtx);
  if (needs_flush) {
    flushNoLock();
  }
}
