// Minimal fake of the Arduino API for host-side tests. See run_tests.sh.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>

#define OUTPUT 1
#define INPUT_PULLUP 2
#define HIGH 1
#define LOW 0
#define F(s) (s)

inline long constrain(long x, long a, long b) { return x < a ? a : (x > b ? b : x); }
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

extern unsigned long fake_millis;
extern int fake_analog;
extern int fake_button;
extern int fake_pwm;

inline unsigned long millis() { return fake_millis; }
inline unsigned long micros() { return fake_millis * 1000; }
inline int analogRead(uint8_t) { return fake_analog; }
inline void analogWrite(uint8_t, int v) { fake_pwm = v; }
inline int digitalRead(uint8_t) { return fake_button; }
inline void pinMode(uint8_t, uint8_t) {}
inline long random(long lo, long hi) { return hi <= lo ? lo : lo + std::rand() % (hi - lo); }
inline void randomSeed(unsigned long) { std::srand(42); }   // deterministic tests
inline uint32_t esp_random() { return 0x00abcdefu; }

struct FakeSerial {
  bool echo = false;
  void begin(long) {}
  template <typename T> void print(T v) { if (echo) std::cout << v; }
  template <typename T> void println(T v) { if (echo) std::cout << v << "\n"; }
};
extern FakeSerial Serial;
