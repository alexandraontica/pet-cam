import { useState, useEffect, useRef } from "react";
import { useNavigate } from "react-router";
import { Button } from "../components/button";
import { Card, CardContent, CardHeader, CardTitle } from "../components/card";
import { Badge } from "../components/badge";
import { Switch } from "../components/switch";
import { Label } from "../components/label";
import {
  Camera,
  PawPrint,
  LogOut,
  Video,
  VideoOff,
  Maximize2,
  Minimize2,
  Bell,
  Settings,
} from "lucide-react";
import { toast } from "sonner";

export function CameraStream() {
  const navigate = useNavigate();
  const [currentUser, setCurrentUser] = useState<any>(null);
  const [isStreaming, setIsStreaming] = useState(true);
  const [motionDetection, setMotionDetection] = useState(true);
  const [isFullscreen, setIsFullscreen] = useState(false);
  const [activity, setActivity] = useState<string[]>([]);
  const streamContainerRef = useRef<HTMLDivElement | null>(null);
  const activityIntervalRef = useRef<number | null>(null);

  useEffect(() => {
    // Check if user is logged in
    const user = localStorage.getItem("petcam_current_user");
    if (!user) {
      toast.error("Please login first!");
      navigate("/");
      return;
    }
    setCurrentUser(JSON.parse(user));

    activityIntervalRef.current = window.setInterval(() => {
      if (motionDetection) {
        setActivity((prev) => {
          const newActivity = [...prev, `${new Date().toLocaleTimeString()} - 🐾 Movement detected`];
          return newActivity.slice(-5);
        });
      }
    }, 5000);

    return () => {
      if (activityIntervalRef.current) {
        clearInterval(activityIntervalRef.current);
      }
    };
  }, [navigate, motionDetection]);

  useEffect(() => {
    const onFullscreenChange = () => {
      setIsFullscreen(document.fullscreenElement === streamContainerRef.current);
    };

    document.addEventListener("fullscreenchange", onFullscreenChange);
    return () => {
      document.removeEventListener("fullscreenchange", onFullscreenChange);
    };
  }, []);

  const handleLogout = () => {
    localStorage.removeItem("petcam_current_user");
    toast.success("Logged out successfully! 👋");
    navigate("/");
  };

  const toggleStream = () => {
    setIsStreaming(!isStreaming);
    toast.info(isStreaming ? "Stream paused" : "Stream resumed");
  };

  const handleFullscreen = async () => {
    try {
      if (!document.fullscreenElement && streamContainerRef.current) {
        await streamContainerRef.current.requestFullscreen();
      } else if (document.fullscreenElement) {
        await document.exitFullscreen();
      }
    } catch {
      toast.error("Could not toggle fullscreen on this browser.");
    }
  };

  return (
    <div className="min-h-screen bg-gradient-to-br from-pink-50 via-purple-50 to-blue-50">
      {/* Header */}
      <header className="bg-white/80 backdrop-blur-sm border-b-2 border-purple-200 shadow-sm">
        <div className="max-w-7xl mx-auto px-4 py-4 flex items-center justify-between">
          <div className="flex items-center gap-3">
            <div className="size-10 bg-gradient-to-br from-pink-400 to-purple-500 rounded-full flex items-center justify-center">
              <Camera className="size-6 text-white" />
            </div>
            <div>
              <h1 className="font-bold bg-gradient-to-r from-pink-500 to-purple-500 bg-clip-text text-transparent">
                PetCam
              </h1>
              <p className="text-sm text-gray-600">Welcome, {currentUser?.name}! 👋</p>
            </div>
          </div>
          <Button
            onClick={handleLogout}
            variant="outline"
            className="border-pink-300 hover:bg-pink-50"
          >
            <LogOut className="size-4 mr-2" />
            Logout
          </Button>
        </div>
      </header>

      <div className="max-w-7xl mx-auto px-4 py-8">
        <div className="grid grid-cols-1 lg:grid-cols-3 gap-6">
          {/* Main Camera Stream */}
          <div className="lg:col-span-2 space-y-4">
            <Card className="border-2 border-purple-200 shadow-xl overflow-hidden">
              <CardHeader className="bg-gradient-to-r from-pink-100 to-purple-100">
                <div className="flex items-center justify-between">
                  <CardTitle className="flex items-center gap-2">
                    <Video className="size-5 text-pink-500" />
                    Felix
                  </CardTitle>
                  <Badge className="bg-green-500 text-white animate-pulse">
                    <span className="size-2 bg-white rounded-full mr-2 inline-block" />
                    LIVE
                  </Badge>
                </div>
              </CardHeader>
              <CardContent className="p-0">
                <div ref={streamContainerRef} className="bg-black">
                  <div className={`relative bg-gray-900 ${isFullscreen ? "h-[calc(100vh-3rem)]" : "aspect-video"}`}>
                    {isStreaming ? (
                      <>
                        <img
                          src="https://images.unsplash.com/photo-1586731790190-c607b3c32150?crop=entropy&cs=tinysrgb&fit=max&fm=jpg&ixid=M3w3Nzg4Nzd8MHwxfHNlYXJjaHwxfHxjdXRlJTIwY2F0JTIwc2xlZXBpbmd8ZW58MXx8fHwxNzc0ODY0Njg5fDA&ixlib=rb-4.1.0&q=80&w=1080&utm_source=figma&utm_medium=referral"
                          alt="Felix the cat"
                          className="w-full h-full object-cover"
                        />
                        {/* Simulated timestamp overlay */}
                        <div className="absolute top-4 left-4 bg-black/60 text-white px-3 py-1 rounded text-sm font-mono">
                          {new Date().toLocaleTimeString()}
                        </div>
                        {/* Motion detection indicator */}
                        {motionDetection && (
                          <div className="absolute top-4 right-4 bg-red-500/80 text-white px-3 py-1 rounded text-sm flex items-center gap-2 animate-pulse">
                            <Bell className="size-4" />
                            Motion Detection ON
                          </div>
                        )}
                      </>
                    ) : (
                      <div className="w-full h-full flex items-center justify-center">
                        <div className="text-center text-gray-400">
                          <VideoOff className="size-16 mx-auto mb-4" />
                          <p>Stream Paused</p>
                        </div>
                      </div>
                    )}
                  </div>

                  {/* Controls */}
                  <div
                    className={`bg-gradient-to-r from-pink-100 to-purple-100 flex items-center justify-center ${
                      isFullscreen ? "p-2 gap-2" : "p-4 gap-4"
                    }`}
                  >
                    <Button
                      onClick={toggleStream}
                      size={isFullscreen ? "sm" : "lg"}
                      className={
                        isStreaming
                          ? "bg-gradient-to-r from-pink-400 to-purple-500 hover:from-pink-500 hover:to-purple-600"
                          : "bg-gray-400 hover:bg-gray-500"
                      }
                    >
                      {isStreaming ? (
                        <>
                          <Video className="size-5 mr-2" />
                          Pause
                        </>
                      ) : (
                        <>
                          <VideoOff className="size-5 mr-2" />
                          Resume
                        </>
                      )}
                    </Button>
                    <Button onClick={handleFullscreen} variant="outline" size={isFullscreen ? "sm" : "lg"}>
                      {isFullscreen ? <Minimize2 className="size-5" /> : <Maximize2 className="size-5" />}
                    </Button>
                  </div>
                </div>
              </CardContent>
            </Card>

          </div>

          {/* Sidebar */}
          <div className="space-y-4">
            {/* Activity Log */}
            <Card className="border-2 border-purple-200">
              <CardHeader className="bg-gradient-to-r from-pink-50 to-purple-50">
                <CardTitle className="flex items-center gap-2">
                  <PawPrint className="size-5 text-pink-500" />
                  Recent Activity
                </CardTitle>
              </CardHeader>
              <CardContent className="pt-4">
                {activity.length > 0 ? (
                  <div className="space-y-2">
                    {activity.map((act, idx) => (
                      <div
                        key={idx}
                        className="flex items-center gap-2 p-2 bg-purple-50 rounded text-sm"
                      >
                        {act}
                      </div>
                    ))}
                  </div>
                ) : (
                  <p className="text-gray-500 text-sm">No recent activity detected</p>
                )}
              </CardContent>
            </Card>

            {/* Settings */}
            <Card className="border-2 border-purple-200">
              <CardHeader className="bg-gradient-to-r from-pink-50 to-purple-50">
                <CardTitle className="flex items-center gap-2">
                  <Settings className="size-5 text-pink-500" />
                  Settings
                </CardTitle>
              </CardHeader>
              <CardContent className="pt-4 space-y-4">
                <div className="flex items-center justify-between">
                  <Label htmlFor="motion-detection" className="flex items-center gap-2 cursor-pointer">
                    <Bell className="size-4 text-purple-500" />
                    Notify me when motion is detected
                  </Label>
                  <Switch
                    id="motion-detection"
                    checked={motionDetection}
                    onCheckedChange={(checked) => {
                      setMotionDetection(checked);
                      toast.info(
                        checked ? "Motion alerts enabled 🔔" : "Motion alerts disabled 🔕"
                      );
                    }}
                  />
                </div>
              </CardContent>
            </Card>
          </div>
        </div>
      </div>
    </div>
  );
}
