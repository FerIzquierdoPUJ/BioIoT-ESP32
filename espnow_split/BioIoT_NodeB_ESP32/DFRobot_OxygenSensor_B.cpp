/*!
 * @file DFRobot_OxygenSensor.cpp
 * @brief Define the basic struct of DFRobot_OxygenSensor class, the implementation of basic method
 * @copyright	Copyright (c) 2010 DFRobot Co.Ltd (http://www.dfrobot.com)
 * @license The MIT License (MIT)
 * @author ZhixinLiu(zhixin.liu@dfrobot.com)
 * @version V1.0
 * @date 2022-08-02
 * @url https://github.com/DFRobot/DFRobot_OxygenSensor
 */
#include "DFRobot_OxygenSensor_B.h"

DFRobot_OxygenSensor::DFRobot_OxygenSensor(TwoWire *pWire)
{
  this->_pWire = pWire;
}

DFRobot_OxygenSensor::~DFRobot_OxygenSensor()
{
  this->_pWire = NULL;
}

bool DFRobot_OxygenSensor::begin(uint8_t addr)
{
  _addr = addr;
  // Wire is initialized by the sketch with its explicit SDA/SCL pins.
  uint8_t version = 0;
  if (!readChecked(VERSION_REGISTER, &version, 1, 0)) return false;
  _version = version == 0x01 ? eNewVersion : eOldVersion;
  return true;
}

void DFRobot_OxygenSensor::readFlash()
{
  uint8_t value[2] = {};
  if (!readChecked(GET_KEY_REGISTER, value, 2, 50)) return;
  uint16_t key = (uint16_t(value[1]) << 8) | value[0];
  _Key = key == 0 ? 20.9f / 120.0f : float(key) / 1000.0f;
}

void DFRobot_OxygenSensor::i2cWrite(uint8_t reg, uint8_t data)
{
  _pWire->beginTransmission(_addr);
  _pWire->write(reg);
  _pWire->write(data);
  _wireError = _pWire->endTransmission();
  _readCount = 0;
  _communicationOk = _wireError == 0;
}

void DFRobot_OxygenSensor::calibrate(float vol, float mv)
{
  uint16_t keyValue = vol * 10;
  uint8_t keytemp = 0;
  if(mv < 0.000001 && mv > (-0.000001) ) {
    keytemp = ((uint8_t)keyValue & 0xff);
    i2cWrite(USER_SET_REGISTER, keytemp);
  }else {
    keyValue = (vol / mv) * 1000;
    if(_version == eOldVersion){
      if(keyValue <= 255){
        keytemp = ((uint8_t)keyValue & 0xff);
      }else{
        keytemp = 255;
      }
      i2cWrite(AUTUAL_SET_REGISTER, keytemp);
    }else{
      _pWire->beginTransmission(_addr);
      _pWire->write(AUTUAL_SET_REGISTER_);
      _pWire->write(keyValue & 0xFF);
      _pWire->write((keyValue >> 8) & 0xFF);
      _pWire->endTransmission();
    }
  }
}

float DFRobot_OxygenSensor::getOxygenData(uint8_t collectNum)
{
  if (collectNum == 0 || collectNum > OCOUNT) { _communicationOk = false; return NAN; }
  readFlash();
  if (!_communicationOk) { _samples = 0; return NAN; }
  uint8_t rxbuf[3] = {};
  if (!readChecked(OXYGEN_DATA_REGISTER, rxbuf, 3, 100)) { _samples = 0; return NAN; }
  for (uint8_t j = collectNum - 1; j > 0; --j) oxygenData[j] = oxygenData[j - 1];
  oxygenData[0] = _Key * (float(rxbuf[0]) + float(rxbuf[1]) / 10.0f + float(rxbuf[2]) / 100.0f);
  if (_samples < collectNum) ++_samples;
  if (_samples > collectNum) _samples = collectNum;
  return getAverageNum(oxygenData, _samples);
}

float DFRobot_OxygenSensor::getAverageNum(float bArray[], uint8_t len)
{
  uint8_t i = 0;
  double bTemp = 0;
  for(i = 0; i < len; i++) {
    bTemp += bArray[i];
  }
  return bTemp / (float)len;
}

eProbeLife_t DFRobot_OxygenSensor::checkProbeLife(void)
{
  int8_t value = 0;
  if(_version == eOldVersion){
    return eVersionError;
  }
  _pWire->beginTransmission(_addr);
  _pWire->write(PROBE_LIFE_REGISTER);
  _pWire->endTransmission();
  _pWire->requestFrom(_addr, (uint8_t)1);
  if(_pWire->available()){
    value = _pWire->read();
  }
  return (eProbeLife_t)value;
}

uint8_t DFRobot_OxygenSensor::getVersion(void)
{
  uint8_t value = 0;
  _pWire->beginTransmission(_addr);
  _pWire->write(VERSION_REGISTER);
  _pWire->endTransmission();
  _pWire->requestFrom(_addr, (uint8_t)1);
  if(_pWire->available()){
    value = _pWire->read();
  }
  return value;
}

float DFRobot_OxygenSensor::getCurrentData(void)
{
  float crrData = 0.0;
  uint8_t rxbuf[10]={0}, k = 0;

  _pWire->beginTransmission(_addr);
  _pWire->write(OXYGEN_DATA_REGISTER);
  _pWire->endTransmission();
  delay(100);
  _pWire->requestFrom(_addr, (uint8_t)3);
  while (_pWire->available()){
    rxbuf[k++] = _pWire->read();
  }
  crrData = ((float)rxbuf[0]) + ((float)rxbuf[1] / 10.0) + ((float)rxbuf[2] / 100.0);
  delay(50);
  return crrData;
}

// A short read is not a fabricated Wire error: wireError can be 0 while
// communicationOk=false and lastReadCount < requested bytes.
bool DFRobot_OxygenSensor::readChecked(uint8_t reg, uint8_t *data, uint8_t count, uint16_t waitMs)
{
  _pWire->beginTransmission(_addr);
  _pWire->write(reg);
  _wireError = _pWire->endTransmission();
  _readCount = 0;
  _communicationOk = false;
  if (_wireError != 0) return false;
  if (waitMs) delay(waitMs);
  _readCount = _pWire->requestFrom(_addr, count);
  uint8_t i = 0;
  while (_pWire->available() && i < count) data[i++] = _pWire->read();
  while (_pWire->available()) _pWire->read();
  _communicationOk = _readCount == count && i == count;
  return _communicationOk;
}


// ---- BioIoT nodo B: variantes por etapas de readChecked()/readFlash() ----
bool DFRobot_OxygenSensor::writeRegister(uint8_t reg)
{
  _pWire->beginTransmission(_addr);
  _pWire->write(reg);
  _wireError = _pWire->endTransmission();
  _readCount = 0;
  _communicationOk = _wireError == 0;
  return _communicationOk;
}

bool DFRobot_OxygenSensor::collect(uint8_t *data, uint8_t count)
{
  _readCount = _pWire->requestFrom(_addr, count);
  uint8_t i = 0;
  while (_pWire->available() && i < count) data[i++] = _pWire->read();
  while (_pWire->available()) _pWire->read();
  _communicationOk = _readCount == count && i == count;
  return _communicationOk;
}

bool DFRobot_OxygenSensor::finishKeyRead()
{
  uint8_t value[2] = {};
  if (!collect(value, 2)) return false;
  uint16_t key = (uint16_t(value[1]) << 8) | value[0];
  _Key = key == 0 ? 20.9f / 120.0f : float(key) / 1000.0f;
  return true;
}

bool DFRobot_OxygenSensor::finishDataRead(float &raw)
{
  uint8_t rxbuf[3] = {};
  raw = NAN;
  if (!collect(rxbuf, 3)) return false;
  raw = _Key * (float(rxbuf[0]) + float(rxbuf[1]) / 10.0f + float(rxbuf[2]) / 100.0f);
  return true;
}
