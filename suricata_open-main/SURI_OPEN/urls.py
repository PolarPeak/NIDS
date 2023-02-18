from django.urls import include, path
from . import views

from suricata.views import rule
urlpatterns = [
    #path('admin/', admin.site.urls),
    path('', rule),
    path('index/', rule),
    path('suricata/', include('suricata.urls')),
]

handler404 = "SURI_OPEN.views.page_not_found"
handler500 = "SURI_OPEN.views.page_error"
