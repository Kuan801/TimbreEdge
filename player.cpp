#include "player.h"

void Player::begin(AudioSynthAdditive *synth) {
  _synth = synth;
  load();
}

void Player::load() {
  if (_playing) stop();
  _n          = buildScore(_notes, TC_MAX_NOTES);
  _totalTicks = scoreTotalTicks();
  Serial.printf("[SCORE] %s：%d 個音符，%u tick\n", scoreName(), _n, _totalTicks);
}

void Player::start(float bpm) {
  if (!_synth) return;
  _cursor  = 0;
  _tick    = 0;
  _playing = true;
  _tickUs  = (uint32_t)(60000000.0f / (bpm * TC_TICKS_PER_BEAT));
  _lastUs  = micros();
  _accUs   = 0;
  for (unsigned i = 0; i < sizeof(_pend) / sizeof(_pend[0]); i++) _pend[i].used = false;

  Serial.printf("[PLAY] 開始演奏，%.0f BPM，約 %.1f 秒\n",
                bpm, _totalTicks * _tickUs / 1e6f);
  fireTick();
}

void Player::stop() {
  _playing = false;
  if (_synth) _synth->allNotesOff();
  for (unsigned i = 0; i < sizeof(_pend) / sizeof(_pend[0]); i++) _pend[i].used = false;
  Serial.println(F("[PLAY] 停止"));
}

void Player::scheduleOff(uint16_t offTick, uint8_t midi) {
  for (unsigned i = 0; i < sizeof(_pend) / sizeof(_pend[0]); i++) {
    if (!_pend[i].used) { _pend[i] = {offTick, midi, true}; return; }
  }

  _synth->noteOff((float)_pend[0].midi);
  _pend[0] = {offTick, midi, true};
}

void Player::fireTick() {

  for (unsigned i = 0; i < sizeof(_pend) / sizeof(_pend[0]); i++) {
    if (_pend[i].used && _pend[i].offTick <= _tick) {
      _synth->noteOff((float)_pend[i].midi);
      _pend[i].used = false;
    }
  }

  while (_cursor < _n && _notes[_cursor].tick == _tick) {
    const ScoreNote &sn = _notes[_cursor];
    float pan = (sn.part == 0) ? 0.62f : (sn.part == 1 ? 0.38f : 0.50f);

    if (sn.part == 1) pan = (sn.midi % 2) ? 0.32f : 0.68f;
    _synth->noteOn((float)sn.midi, sn.vel / 127.0f, pan);
    scheduleOff((uint16_t)(sn.tick + sn.dur), sn.midi);
    _cursor++;
  }
}

void Player::service() {
  if (!_playing) return;

  uint32_t now = micros();
  _accUs += (now - _lastUs);
  _lastUs = now;

  while (_accUs >= _tickUs) {
    _accUs -= _tickUs;
    _tick++;
    fireTick();

    if (_tick > _totalTicks) {
      _playing = false;
      _synth->allNotesOff();
      Serial.println(F("[PLAY] 演奏結束"));
      return;
    }
  }
}
