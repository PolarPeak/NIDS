from django.db.models.fields import NullBooleanField
from django.shortcuts import render
from .models import suri_log
import json
from django.http import HttpResponse
from django.db import connections
from elasticsearch import Elasticsearch
import json
import re,requests
from .config import *

def rule(request):
    data={"flag_one":"suricata","flag_two":"rule"}
    return render(request, 'suricata/rule.html', data)

def dasboard(request):
      
    #data={"flag_one":"suricata","flag_two":"rule"}
    return render(request, 'suricata/index.html')

def logs(request):
    es = Elasticsearch(hosts=es_ip,port=es_port)
    body = {
      "range": {
            "alert.severity":{
            "lte":4
             }
            }
      }

    logs = []
    test = es.search(index = "suricata-1.1.0-*", doc_type = "_doc",size=10000,query=body)
    for test in test["hits"]["hits"]:
        _id = test["_id"]
        time = test["_source"]["@timestamp"]
        time = time.replace('T'," ").replace("Z","")
        in_iface = test["_source"]["in_iface"]

        dest_hostname = test["_source"]["dest_hostname"]
        dest_port = test["_source"]["dest_port"]
        src_ip = test["_source"]["src_ip"]
        src_port = test["_source"]["src_port"]

        attack_ip = dest_hostname+":"+str(dest_port)
        victim_ip = src_ip+":"+str(src_port)

        level = test["_source"]["log"]["severity"]
        vul = test["_source"]["alert"]["signature"]
        vul = vul.replace('ET','')
        logs.append({"id":_id,"time":time,"in_iface":in_iface,"attack_ip":attack_ip,"victim_ip":victim_ip,"level":level,"vul":vul})
        
    return render(request, 'suricata/log.html',{"flag_one": "suricata", "flag_two": "logs","data":logs})

def get_log_by_id(request,id):
      es = Elasticsearch(hosts=es_ip,port=es_port)
      body = {
        "match": {
            "_id": id
        }
      }
      test = es.search(index="suricata-1.1.0-*",doc_type = "_doc",size=5000,query=body)

      log = test["hits"]["hits"][0]
      time = log["_source"]["@timestamp"]
      time = time.replace('T'," ").replace("Z","")
      # print("时间:"+time.replace('T'," ").replace("Z",""))
      service_name = log["_source"]["service_name"]
      server_ip = log["_source"]["server_ip"]
      service_port = log["_source"]["service_port"]
      server_ip_port = server_ip + service_port
      # print("协议:"+service_name)
      # ip_version = log["_source"]["ip_version"]
      # print("IP版本:"+ip_version)
      src_hostname = log["_source"]["src_ip"]
      # print("攻击源IP："+src_hostname)
      src_port = log["_source"]["src_port"]
      # print("攻击源端口："+str(src_port))
      level = log["_source"]["log"]["severity"]
      # print("安全等级："+level)
      vul = log["_source"]["alert"]["signature"]
      flow_id = log["_source"]["flow_id"]
      # protocol = log["_source"]["http"]["protocol"]
      # http_method = log["_source"]["http"]["http_method"]
      # url = log["_source"]["http"]["url"]
      # status = log["_source"]["http"]["status"]
      # hostname = log["_source"]["http"]["hostname"]
      # http_port = log["_source"]["http"]["http_port"]
      # ip = hostname + http_port
      # http_user_agent = log["_source"]["http"]["http_user_agent"]

      log1 = str(log)
      protocol = None
      http_method = None
      url = None
      status = None
      hostname = None
      http_port = None
      ip = None
      http_user_agent = None
      http_refer = None
      http_request_body = None
      http_response_body = None


      if 'files' in log1:
            payload = log["_source"]["files"][0]["filename"]
      else:
            payload = None
      if "HTTP/1.1" in log1:
            protocol = log["_source"]["http"]["protocol"]
            http_method = log["_source"]["http"]["http_method"]
            url = log["_source"]["http"]["url"]
            if "status" in log1:
                  status = log["_source"]["http"]["status"]
                  if status == 200:
                        info = get_flow_id(flow_id)
                        if info:
                              http_request_body = info[0]
                              http_response_body = info[1]
                              http_refer = info[2]
                        else:
                              pass

            if "http_port" in log1:
                  hostname = log["_source"]["http"]["hostname"]
            if "http_port" in log1:
                  http_port = log["_source"]["http"]["http_port"]
                  ip = str(hostname) + ":" + str(http_port)
            if "http_user_agent" in log1:
                  http_user_agent = log["_source"]["http"]["http_user_agent"]
            # if 'http_refer' in log1:
            #     http_refer = log["_source"]["http"]["http_refer"]
            # else:
            #       http_refer = None
            # if 'http_request_body' in log1:
            #       http_request_body = log["_source"]["http"]["http_request_body"]
            # else:
            #       http_request_body = None


      logs = {"time":time,"service_name":service_name,"src_hostname":src_hostname,"src_port":src_port,"server_ip_port":server_ip_port,"level":level,"vul":vul,"payload":payload,"protocol":protocol,"http_method":http_method,"url":url,"status":status,"ip":ip,"http_user_agent":http_user_agent,"http_refer":http_refer,"http_request_body":http_request_body,"http_response_body":http_response_body}

      return render(request,'suricata/logstat.html',{"flag_one": "suricata", "flag_two": "logs","data":logs})

def get_flow_id(id):
      es = Elasticsearch(hosts=es_ip,port=es_port)
      body = {
            "term": {
                  "flow_id":id

            }
      }
      test = es.search(index="suricata-1.1.0-*",doc_type = "_doc",size=10000,query=body)
      request_body = None
      response_body = None
      http_refer = None
      total = test["hits"]["total"]["value"]
      print(total)
      if total:
            for test in test["hits"]["hits"]:
                  info = str(test)
                  if "http_request_body" in info:
                        request_body = test["_source"]["http"]["http_request_body"]

                  if "http_response_body" in info:
                        response_body = test["_source"]["http"]["http_response_body"]
                  if "http_refer" in info:
                        http_refer = test["_source"]["http"]["http_refer"]

      else:
            return False

      return request_body,response_body,http_refer
      

def threat(request):
      data={"flag_one":"suricata","flag_two":"threat"}
      threats = {}
      if request.method == 'POST':
            ip = request.POST.get('ip')
            if not re.match(r'^(([0-9]{1,3}).){3}[0-9]{1,3}$', ip, re.I):
                  return render(request, '/templates/500.html')
            r = requests.post('https://api.threatbook.cn/v3/scene/ip_reputation', {
                  'apikey': "",
                  'lang': 'zh',
                  'resource': ip,
                  }).json()

            if r['response_code'] == 0:  # 数据正常
                  dr = r['data']
                  for name in dr:  # name是查询的ip 这个循环只有一次
                        info = dr[name]
                        severity = info['severity']     # 危害程度  严重/高/中/低/无危胁
                        confidence_level = info['confidence_level'] #IP可信度
                        judgmentsarr = info['judgments']   # 类型数组
                        ibasic = info['basic']
                        blocation = ibasic['location']
                        carrier = ibasic['carrier']       # 运营商
                        country = blocation['country']    # 国家
                        province = blocation['province']  # 省
                        city = blocation['city']          # 城市
                        locationname = '%s %s %s' % (country, province, city)
                        judgments = ''
                        for j in judgmentsarr:
                              judgments = '%s %s' % (judgments, j)
                        threats = {"ip":ip,"severity":severity,"locationname":locationname,"carrier":carrier,"judgments":judgments,"confidence_level":confidence_level}
                  # return render(request,'suricata/xthreat.html',{"flag_one": "suricata", "flag_two": "threat","threats":threats})

                  # threats = json.dumps({
                  # 'ip': ip,
                  # 'severity': severity,
                  # 'locationname': locationname,
                  # 'judgments': judgments,
                  # }, ensure_ascii=False)
            data = {"flag_one": "suricata","flag_two": "threat","threats":threats}
      return render(request, 'suricata/morlist.html', {"flag_one": "suricata", "flag_two": "threat","threats":threats})




































# def monitor(request):
#     sobjs = SuriMonitor.objects.filter(project="suricata")
#     flag="new"

#     data = {"flag_one": "suricata", "flag_two": "monitor","flag":flag}
#     return render(request, 'suricata/monitor.html', data)

# def edit_mornitor(request,suid):
#     sobjs = SuriMonitor.objects.filter(project="suricata",id=suid)
#     flag="edit"
#     sobj=sobjs[0]
#     data = {"flag_one": "suricata", "flag_two": "monitor","flag":flag,"sobj":sobj}
#     return render(request, 'suricata/monitor.html', data)

# def savesuri(request):
#     sid=request.POST.get("sid")
#     times = request.POST.get("times")
#     cnts = request.POST.get("cnts")
#     tips = request.POST.get("tips")
#     flag = request.POST.get("flag")
#     suid = request.POST.get("suid")
#     #print(sid,times,cnts)
#     code=0
#     try:
#         #SuriMonitor.objects.filter(project="suricata").delete()
#         if flag=="edit":
#             s = SuriMonitor.objects.filter(id=int(suid))[0]
#         else:
#             s = SuriMonitor()
#         s.project="suricata"
#         s.sid = int(sid)
#         s.times = int(times)
#         s.cnts = int(cnts)
#         s.tips = tips
#         s.save()
#     except Exception as e:
#         print(e)
#         print("$$$$$$$$$$")
#         code=1
#     ret={"code":code}
#     return HttpResponse(json.dumps(ret, ensure_ascii=False), content_type="application/json,charset=utf-8")

# def delmor(request,suid):

#     print(suid)
#     code=0

#     if suid:
#         aobj = sobjs = SuriMonitor.objects.filter(project="suricata",id=suid)

#         aobj.delete()

#     ret={"code":code}
#     return HttpResponse(json.dumps(ret, ensure_ascii=False), content_type="application/json,charset=utf-8")



# def monitorSave(request):
#     ret={"code":200}
#     return HttpResponse(json.dumps(ret, ensure_ascii=False), content_type="application/json,charset=utf-8")
#     data = {}
#     recode = data['recode']
#     retdata = {}
#     retdata["code"] = recode
#     retdata["remsg"] = data['remsg']
#     if recode == 0:
#         robj = RedisHelper()
#         for url in data["url_list"]:
#             t = TaskInfo()
#             t.command = data['task_command']
#             t.url = url
#             t.method = data['task_method']
#             t.postdata = data['task_postdata']
#             t.plugins = data['task_plugins']
#             t.crawler = data['task_crawler']
#             t.user = data['task_user']
#             t.status=1
#             t.save()
#             tid = t.id
#             tcrawf=False
#             if data['task_crawler']=='1':
#                 tcrawf=True
#             tmsg={}
#             tmsg["taskid"]=str(tid)
#             tmsg["user"] = data['task_user']
#             tmsg["command"] = data['task_command']
#             tmsg["url"] = url
#             tmsg["request-method"] = data['task_method']
#             tmsg["post-data"] = data['task_postdata']
#             tmsg["plugins"] = data['task_plugins']
#             tmsg["basic-crawler"] = tcrawf

#             robj.pushTask(tmsg)

#             print(tid)

#     return HttpResponse(json.dumps(retdata, ensure_ascii=False), content_type="application/json,charset=utf-8")

