#!/usr/bin/python
# -*- coding: UTF-8 -*-
import smtplib
from email.mime.text import MIMEText
from email.header import Header
import pymysql
from config import *
from elasticsearch import Elasticsearch
import json

import re
import time
import datetime
class SmtpEmail(object):
    def __init__(self):
        self.host = SMTP_HOST
        self.port = SMTP_PORT
        self.sender=SMTP_SENDER
        self.es_receivers = RECEIVERS
        self.smtpObj = self.getSmtp()
    def getSmtp(self):
        smtpObj = smtplib.SMTP(self.host,self.port)
        return smtpObj
    def sendSuriEmail(self,estr,titlestr):

        message = self.getSuriMessage(estr,titlestr)
        print(message)
        reces = self.es_receivers
        try:
            self.smtpObj.sendmail(self.sender, reces, message.as_string())
            print( "邮件发送成功")
        except smtplib.SMTPException as e:
            print(e)
            print("Error: 无法发送邮件")
        self.smtpObj.quit()
    def getSuriMessage(self,estr,titlestr):

        ms_str = '<p><span style="color:red"> suricata 监控:  </span></p>'
        ms_str += '<br>'
        ms_str += '<p>'+titlestr+'</p>'
        ms_str += '<br>'

        if estr:
            ms_str += '<br>'
            ms_str += '<p>suricataip与pafirewall联动信息</p>'
            ms_str += estr
        ms_str += '<br>'

        message = MIMEText(ms_str, 'html', 'utf-8')

        message['From'] = self.sender
        message['To'] = ";".join(self.es_receivers)
        subject = 'suricata监控'
        message['Subject'] = Header(subject, 'utf-8')
        return message
    def sendPer5Email(self,tablestr,dataCount,endtime):
        message = self.getPer5Message(tablestr, dataCount,endtime)
        print(message)
        reces = self.es_receivers
        try:
            self.smtpObj.sendmail(self.sender, reces, message.as_string())
            print("邮件发送成功")
        except smtplib.SMTPException as e:
            print(e)
            print("Error: 无法发送邮件")
        self.smtpObj.quit()
    def getPer5Message(self,tablestr,dataCount,endtime):
        ms_str = '<p>5分钟产生报警总条数：' + str(dataCount) + '</p>'
        endtimestr=endtime.strftime("%Y-%m-%d %H:%M:%S")
        starttimestr=(endtime-datetime.timedelta(minutes=5)).strftime("%Y-%m-%d %H:%M:%S")
        ms_str += '<p>时间段：' + str(starttimestr) +"   ——   "+str(endtimestr)+ '</p>'
        ms_str += '<p><span style="color:red"> suricata 5分钟内产生的报警 ： </span></p>'

        ms_str += '<br>'
        ms_str += tablestr
        ms_str += '<br>'

        message = MIMEText(ms_str, 'html', 'utf-8')

        message['From'] = self.sender
        message['To'] = ";".join(self.es_receivers)
        subject = 'suricata5分钟监控'
        message['Subject'] = Header(subject, 'utf-8')
        return message
    def uniqueEmail(self,inputlist=[]):
        outputlist=[]
        for e in inputlist:
            if e in outputlist:
                pass
            else:
                outputlist.append(e)
        return outputlist

def suri_monitor():
    conn = pymysql.connect(host=db_host,port=db_port,user =db_user, password =db_password,database =db_name,charset ="utf8")
    cursor = conn.cursor()

    sql_str = """select sid,times,cnts,tips from suri_monitor where project="suricata";"""

    cursor.execute(sql_str)

    rs = cursor.fetchall()
    cursor.close()
    conn.close()
    if not rs:
        return
    surilist=[]
    titlelist=[]
    tablelist=[]
    for r in rs:
        sid=r[0]
        times=r[1]
        cnts=r[2]
        tips=r[3]
        ret_suri=suries(sid,times,cnts)

        if ret_suri:
            titlestr = "<p>SID:%s (%s)  %s分钟时间内出现次数(%s)超过%s次</p>" % (str(sid), str(tips), str(times), str(len(ret_suri)),str(cnts))
            alertstr = "SID:%s %s分钟时间内出现 %s次， 次数超过%s次" % (str(sid), str(times), str(len(ret_suri)),str(cnts))
            ret_pa = getPaThreat(ret_suri)
            tablestr = table_suri(ret_pa)
            titlelist.append(titlestr)
            if tablestr:
                tablestr="<p> SID :%s 关联的防火墙信息</p>"%str(sid)+tablestr
                tablelist.append(tablestr)
            surilist.append({"titlestr":titlestr,"tablestr":tablestr,"sid":sid})
    if titlelist:
        tstr = "".join(titlelist)
        astr="".join(tablelist)
        se = SmtpEmail()
        se.sendSuriEmail(astr, tstr)
pa_str={
  "query":{
    "bool":{
      "filter":[{
        "range": {
          "@timestamp": {
            "gte": "now-24h/s","lt":"now/s"
          }
        }},
        {"term":{"src_ip":"10.60.77.11"}},
        {"term":{"dst_ip":"54.255.254.175"}},
        {"term":{"Source-Port":"43596"}},
        {"term":{"Destination-Port":"22"}}

      ]
    }
  },
  "size": 3
}
def getPaThreat(ret_suri):
    ret_list=[]
    indexs = PA_INDEX
    es = Elasticsearch(es_url)
    for r in ret_suri:
        pa_str["query"]["bool"]["filter"][1]["term"]["src_ip"] = r["src_ip"]
        pa_str["query"]["bool"]["filter"][2]["term"]["dst_ip"] = r["dst_ip"]
        pa_str["query"]["bool"]["filter"][3]["term"]["Source-Port"] = r["src_port"]
        pa_str["query"]["bool"]["filter"][4]["term"]["Destination-Port"] = r["dst_port"]

        est = es.search(index=indexs, body=json.dumps(pa_str))
        dataCount = int(est['hits']['total']['value'])
        if dataCount>0:
            r["threat_name"] = est['hits']['hits'][0]["_source"]["Threat-Content-Name"]
            ret_list.append(r)
        else:
            pa_str["query"]["bool"]["filter"][1]["term"]["src_ip"] = r["dst_ip"]
            pa_str["query"]["bool"]["filter"][2]["term"]["dst_ip"] = r["src_ip"]
            pa_str["query"]["bool"]["filter"][3]["term"]["Source-Port"] = r["dst_port"]
            pa_str["query"]["bool"]["filter"][4]["term"]["Destination-Port"] = r["src_port"]

            est = es.search(index=indexs, body=json.dumps(pa_str))
            dataCount = int(est['hits']['total']['value'])
            if dataCount > 0:
                r["threat_name"] = est['hits']['hits'][0]["_source"]["Threat-Content-Name"]
                ret_list.append(r)

    return ret_list

def table_suri(ret_list):
    if not ret_list:
        return ""
    tablestr = "<table  border=1 cellpadding=0 cellspacing=0 style='border-collapse:collapse'><thead><tr><td>源IP&nbsp;&nbsp;&nbsp;&nbsp;</td><td>源端口&nbsp;&nbsp;&nbsp;&nbsp;</td><td>目的IP&nbsp;&nbsp;&nbsp;&nbsp;</td><td>目的端口&nbsp;&nbsp;&nbsp;&nbsp;</td><td>threat name&nbsp;&nbsp;&nbsp;&nbsp;</td></tr></thead><tbody>"
    for r in ret_list:
        trstr = '''<tr><td> %s&nbsp;&nbsp;&nbsp;&nbsp;</td><td>%s&nbsp;&nbsp;&nbsp;&nbsp;</td><td>%s&nbsp;&nbsp;&nbsp;&nbsp;</td><td>%s&nbsp;&nbsp;&nbsp;&nbsp;</td><td>%s</td></tr>''' % (
        r["src_ip"], r["src_port"], r["dst_ip"], r["dst_port"],r["threat_name"])
        tablestr += trstr
    tablestr += "</tbody></table>"
    return tablestr

def suries(sid,times,cnts):
    suri_str = {
  "query": {
        "bool": {
            "filter": [{
                "range": {
                    "@timestamp": {
                        "gte": "now-5m/s","lt":"now/s"
                    }
                }
            },
            {"match":{"message":"2018904"}}]
        }
    },
  "size":100
}

    suri_str["query"]["bool"]["filter"][1]["match"]["message"] = str(sid)
    suri_str["query"]["bool"]["filter"][0]["range"]["@timestamp"]["gte"] = "now-"+str(times)+"m/s"
    indexs = SURI_INDEX
    loopflag = 0

    while loopflag<3:
        es = Elasticsearch(es_url)
        est = es.search(index=indexs, scroll='10s', body=json.dumps(suri_str))
        dataCount = int(est['hits']['total']['value'])
        if dataCount>0:
            break
        loopflag +=1

    scroll_size=dataCount
    size = 100
    sid = est['_scroll_id']
    ret_list = []
    if dataCount <= cnts:
        return ret_list

    while (scroll_size > 0):
        for mele in est['hits']['hits']:
            eled = {}
            ele = mele["_source"]
            eled["src_ip"] = ele["src_ip"]
            eled["src_port"] = ele["src_port"]
            eled["dst_ip"] = ele["dst_ip"]
            eled["dst_port"] = ele["dst_port"]
            eled["message"] = ele["message"]
            ret_list.append(eled)

        est = es.scroll(scroll_id=sid, scroll='10s')
        sid = est['_scroll_id']
        scroll_size = scroll_size - size
    return ret_list
def suri_per5():
    suri_per_str = {
        "query": {
            "bool": {
                "filter": [{
                    "range": {
                        "@timestamp": {
                            "gte": "now-5m/s", "lt": "now/s"
                        }
                    }
                }]
            }
        },
        "size": 50
    }
    indexs = SURI_INDEX
    loopflag = 0
    dataCount=0
    endtime=datetime.datetime.now()
    while loopflag < 3:
        es = Elasticsearch(es_url)
        est = es.search(index=indexs, body=json.dumps(suri_per_str))
        dataCount = int(est['hits']['total']['value'])
        if dataCount > 0:
            break
        loopflag += 1
    ret_list=[]
    if dataCount>0:
        jsonfilestr = JSON_FILE
        jf = open(jsonfilestr,encoding='utf-8')
        res=json.load(jf)
        jf.close()
        for mele in est['hits']['hits']:
            eled = {}
            ele = mele["_source"]
            eled["message"] = ele["message"]
            sid = messageGetSid(ele["message"])
            eled["sid"] = sid
            eled["rules"]=res.get(sid,"")
            ret_list.append(eled)
    if ret_list:
        tablestr = "<table  border=1 cellpadding=0 cellspacing=0 style='border-collapse:collapse'><thead><tr><td>message</td><td>sid</td><td>rules</td></tr></thead><tbody>"
        for r in ret_list:
            trstr = '''<tr><td> %s&nbsp;&nbsp;&nbsp;&nbsp;</td><td> %s&nbsp;&nbsp;&nbsp;&nbsp;</td><td> %s&nbsp;&nbsp;&nbsp;&nbsp;</td></tr>''' % (r["message"],r["sid"],r["rules"])
            tablestr += trstr
        tablestr += "</tbody></table>"

        se = SmtpEmail()
        se.sendPer5Email(tablestr,dataCount,endtime)

def messageGetSid(s):
    ret=""
    pattern = ".*\[\d:(?P<name>\d*):\d\].*"
    st = re.findall(pattern,s)
    if st:
        ret = st[0]
    return ret

def rulesToJson():
    rulefilestr=RULE_FILE
    jsonfilestr=JSON_FILE

    rulef = open(rulefilestr,"r",encoding='utf-8')
    rule_dict ={}
    pattern = ".*sid:(?P<name>\d+).*"
    for f in rulef.readlines():
        s = f.strip()
        st = re.findall(pattern, s)

        if st:
            sid = st[0]

            sid=sid.strip()
            if sid:
                rule_dict[sid]=s
    rulef.close()

    fw = open(jsonfilestr, 'w', encoding='utf-8')
    json.dump(rule_dict,fw)

    fw.close()



if __name__ == "__main__":
    suri_monitor()
    suri_per5()
    #rulesToJson()

