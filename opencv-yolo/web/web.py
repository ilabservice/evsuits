from flask import Flask, escape, request, jsonify, g, url_for
import paho.mqtt.client as mqtt
from cerberus import schema_registry, Validator
from celery import Celery
from azure.storage.fileshare import ShareFileClient
import os, yaml, logging, time, datetime, threading, json, subprocess, shlex,re

#import pdb,traceback, sys


"""
va task schema:
  {
    "cameraId": "D72154040",
    "endTime": 1577267418999,
    "image": "http://evcloudsvc.ilabservice.cloud/video/D72154040/1550143347000-1577267418999/firstFrame.jpg",
    "length": 260,
    "startTime": 1550143347000,
    "video": "http://evcloudsvc.ilabservice.cloud/video/D72154040/1550143347000-1577267418999/1550143347000-1577267418999.mp4"
  }

"""
VA_SCHEMAS = {
  'task': {
    'cameraId': {'type': 'string', 'dependencies': ['startTime', 'endTime']},
    'startTime': {'type': 'integer', 'dependencies':'cameraId'},
    'endTime': {'type': 'integer', 'dependencies':'cameraId'},
    'length': {'type': 'integer'},
    'image': {'type': 'string'},
    'video': {'type': 'string'}
  },
}

CONNSTR='DefaultEndpointsProtocol=https;AccountName=ilsvideostablediag;AccountKey=rWeA/cUiWAsDqGHO0lfDB5eDHNZxCChrH0pMvICdNJR6tt+hE2tHlSl9kUEjqyOY6cztPWaaRbbeoI47uNEeWA==;EndpointSuffix=core.chinacloudapi.cn'
SHARENAME='pre-data'

def downloadFile(ipcSn, dirName, fileName):
  file_path=ipcSn+'/'+dirName+'/'+fileName
  print("downloading: {} {} {}".format(ipcSn, dirName, file_path))
  with ShareFileClient.from_connection_string(conn_str=CONNSTR, share_name=SHARENAME, file_path=file_path) as fc:
      with open(fileName, "wb") as f:
          data = fc.download_file()
          data.readinto(f)

class VAMMQTTClient:
  # The callback for when the client receives a CONNACK response from the server.
  @staticmethod
  def on_connect(client, userdata, flags, rc):
    print("Connected with result code "+str(rc))
    # Subscribing in on_connect() means that if we lose the connection and
    # reconnect then subscriptions will be renewed.
    topic = '$queue/video.ai/v1.0/task'
    client.subscribe(topic, qos=1)
    print('subscribed to ', topic)
  #client.subscribe("$queue/video.ai/v1.0/task", qos=1)

  # The callback for when a PUBLISH message is received from the server.
  @staticmethod
  def on_message(client, userdata, msg):
    payload = msg.payload.decode('utf-8')
    print(msg.topic+" "+ payload)
    if userdata:
      try:
        jd = json.loads(payload)
        userdata(jd)
      except Exception as e:
        print('exception in process message:', e)
        #extype, value, tb = sys.exc_info()
        #traceback.print_exc()
        #pdb.post_mortem(tb)

  @staticmethod
  def on_disconnect(client, userdata, rc):
    #topic = "video.ai/v1.0/task"
    #client.publish(topic, payload=None, qos=1, retian=False)
    print("disconnected")
  def __init__(self, callback, host = 'evcloud.ilabservice.cloud', port = 1883):
    '''
    Parameters
    '''
    self.client = mqtt.Client("vamqtt",userdata=callback) #, protocol=mqtt.MQTTv5)
    self.client.on_connect = VAMMQTTClient.on_connect
    self.client.on_message = VAMMQTTClient.on_message
    self.client.connect_async(host, port, 30)
    self.client.loop_start()

app = Flask(__name__,
  static_url_path='', 
  static_folder='web/main/dist')
logger = app.logger

REDIS_ADDR = os.getenv('REDIS', 'redis://localhost:6379')
app.config['broker_url'] = REDIS_ADDR
app.config['result_backend'] = REDIS_ADDR
worker = Celery(app.name, broker=app.config['broker_url'])
worker.conf.update(app.config)
worker.conf.update(
  task_serializer='json',
  #accept_content=['json'],
  result_serializer='json',
  #timezone='Europe/Oslo',
  enable_utc=True)

def take_task(task):
  ret = {'code': 0,'msg': 'ok'}
  print("taking task", json.dumps(task))
  taskValidator = Validator(VA_SCHEMAS['task'])
  if not taskValidator.validate(task):
    ret['code'] = 1
    ret['msg'] = 'invalid request body'
    ret['data'] = taskValidator.errors
  else:
    # process
    video_analysis.apply_async(args=[task])
  logger.info(json.dumps(ret))
  return ret

@app.route('/api/video.ai/v1.0/task', methods=['POST'])
def new_task():
  ret = take_task(request.json)
  return jsonify(ret);

@worker.task
def video_analysis(data):
  ret = {'code': 0, 'msg': 'ok'}
  ret['target'] = data
  print(json.dumps(data))
  # get video
  try:
    if 'cameraId' in data: # azure storage
      ipcSN = data["cameraId"]
      dirName = "{}-{}".format(data["startTime"],data["endTime"])
      fileName = dirName + '.1mp4'
      downloadFile(ipcSN, dirName, fileName)
      print('== ', fileName)
      workd = '/Users/blu/work/opencv-projects/opencv-yolo/'
      cmdLine = workd + 'detector ' + workd + 'web/'+ fileName + ' -c ' + workd
      #cmdLine = '/Users/blu/work/opencv-projects/opencv-yolo/detector /Users/blu/work/opencv-projects/opencv-yolo/web/1550143347000-1577267418999.mp4 -c /Users/blu/work/opencv-projects/opencv-yolo/'
      cmdArgs = shlex.split(cmdLine)
      print(cmdLine, '\n\n', cmdArgs)
      output = subprocess.check_output(cmdArgs)
      print(output)
      # parse
      for line in output.decode('utf-8').split('\n'):
        print("\n=====", line)
        m = re.match(r".*? found (\w+) ([\d\.]+) .*? image: ([_\w\d]+.jpg)", line)
        ret['data'] = {}
        ret['data']['humanDetect'] = {}
        if m:
          ret['data']['humanDetect']['found'] = 1
          ret['data']['humanDetect']['level'] = m.group(2)
          ret['data']['humanDetect']['image'] = m.group(3)
          print('found {}: {}, img: {}'.format(m.group(1), m.group(2), m.group(3)))
        else:
          ret['data']['humanDetect']['found'] = 0

    elif 'video' in data: # http
      pass
    else: # no video
      ret['code'] = 1
      ret['msg'] = 'no video specified'
      return ret
  except Exception as e:
    print("exception in va worker: {}".format(e));
    ret['code'] = -1
    ret['msg'] = str(e)

  return ret


if __name__ == '__main__':
  mq = VAMMQTTClient(take_task)
  app.run(host='0.0.0.0', port = '5000')


