import { useState } from "react";
import { useNavigate } from "react-router";
import { Button } from "../components/button";
import { Input } from "../components/input";
import { Label } from "../components/label";
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from "../components/card";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "../components/tabs";
import { PawPrint, Heart, Camera } from "lucide-react";
import { toast } from "sonner";

type User = {
  name: string;
  username: string;
};

export function Auth() {
  const navigate = useNavigate();
  const [loginUsername, setLoginUsername] = useState("");
  const [loginPassword, setLoginPassword] = useState("");
  const [registerName, setRegisterName] = useState("");
  const [registerUsername, setRegisterUsername] = useState("");
  const [registerPassword, setRegisterPassword] = useState("");

  // Submits the login credentials to the backend API
  const handleLogin = async (e: React.FormEvent) => {
    e.preventDefault();

    try {
      const response = await fetch("/api/login", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
        },
        credentials: "include", // Ensure session cookies are sent and received
        body: JSON.stringify({
          username: loginUsername,
          password: loginPassword,
        }),
      });

      const data = (await response.json()) as { message?: string; user?: User };

      if (!response.ok || !data.user) {
        toast.error(data.message || "Invalid username or password. Please try again!");
        return;
      }

      // Store user info in localStorage for persistence and navigate to stream page
      localStorage.setItem("petcam_current_user", JSON.stringify(data.user));
      toast.success(`Welcome back, ${data.user.name}!`);
      navigate("/camera");
    } catch {
      toast.error("Could not connect to server. Please try again!");
    }
  };

  // Submits new user registration details to the backend API
  const handleRegister = async (e: React.FormEvent) => {
    e.preventDefault();

    try {
      const response = await fetch("/api/register", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
        },
        credentials: "include", // Establish the session cookie on successful registration
        body: JSON.stringify({
          name: registerName,
          username: registerUsername,
          password: registerPassword,
        }),
      });

      const data = (await response.json()) as { message?: string; user?: User };

      if (!response.ok || !data.user) {
        toast.error(data.message || "Could not create account.");
        return;
      }

      // Store session data and redirect to main stream page
      localStorage.setItem("petcam_current_user", JSON.stringify(data.user));
      toast.success(`Welcome to PetCam, ${data.user.name}!`);
      navigate("/camera");
    } catch {
      toast.error("Could not connect to server. Please try again!");
    }
  };

  return (
    <div className="min-h-screen flex items-center justify-center bg-gradient-to-br from-pink-100 via-purple-100 to-blue-100 p-4">
      {/* Floating paw prints decoration */}
      <div className="absolute inset-0 overflow-hidden pointer-events-none">
        <PawPrint className="absolute top-10 left-10 size-8 text-pink-300/30 rotate-12" />
        <PawPrint className="absolute top-20 right-20 size-12 text-purple-300/30 -rotate-12" />
        <PawPrint className="absolute bottom-20 left-20 size-10 text-blue-300/30 rotate-45" />
        <PawPrint className="absolute bottom-10 right-10 size-8 text-pink-300/30 -rotate-45" />
        <Heart className="absolute top-1/4 right-1/4 size-6 text-pink-300/20" />
        <Heart className="absolute bottom-1/3 left-1/3 size-8 text-purple-300/20" />
      </div>

      <div className="w-full max-w-md relative z-10">
        {/* Logo Header */}
        <div className="text-center mb-8">
          <div className="inline-flex items-center justify-center size-20 bg-gradient-to-br from-pink-400 to-purple-500 rounded-full mb-4 shadow-lg">
            <Camera className="size-10 text-white" />
          </div>
          <h1 className="text-4xl font-bold bg-gradient-to-r from-pink-500 via-purple-500 to-blue-500 bg-clip-text text-transparent">
            PetCam
          </h1>
          <p className="text-purple-600 mt-2">Keep an eye on your furry friends</p>
        </div>

        <Tabs defaultValue="login" className="w-full">
          <TabsList className="grid w-full grid-cols-2 bg-white/80 backdrop-blur-sm">
            <TabsTrigger value="login" className="data-[state=active]:bg-gradient-to-r data-[state=active]:from-pink-400 data-[state=active]:to-purple-500 data-[state=active]:text-white">
              Login
            </TabsTrigger>
            <TabsTrigger value="register" className="data-[state=active]:bg-gradient-to-r data-[state=active]:from-pink-400 data-[state=active]:to-purple-500 data-[state=active]:text-white">
              Register
            </TabsTrigger>
          </TabsList>

          <TabsContent value="login">
            <Card className="border-2 border-purple-200 shadow-xl bg-white/90 backdrop-blur-sm">
              <CardHeader>
                <CardTitle className="flex items-center gap-2">
                  <PawPrint className="size-5 text-pink-500" />
                  Welcome Back!
                </CardTitle>
              </CardHeader>
              <CardContent>
                <form onSubmit={handleLogin} className="space-y-4">
                  <div className="space-y-2">
                    <Label htmlFor="login-username">Username</Label>
                    <Input
                      id="login-username"
                      type="text"
                      placeholder="pet_lover"
                      value={loginUsername}
                      onChange={(e) => setLoginUsername(e.target.value)}
                      required
                      className="border-purple-200 focus:border-pink-400"
                    />
                  </div>
                  <div className="space-y-2">
                    <Label htmlFor="login-password">Password</Label>
                    <Input
                      id="login-password"
                      type="password"
                      placeholder="••••••••"
                      value={loginPassword}
                      onChange={(e) => setLoginPassword(e.target.value)}
                      required
                      className="border-purple-200 focus:border-pink-400"
                    />
                  </div>
                  <Button
                    type="submit"
                    className="w-full bg-gradient-to-r from-pink-400 to-purple-500 hover:from-pink-500 hover:to-purple-600 text-white"
                  >
                    <PawPrint className="size-4 mr-2" />
                    Login
                  </Button>
                </form>
              </CardContent>
            </Card>
          </TabsContent>

          <TabsContent value="register">
            <Card className="border-2 border-purple-200 shadow-xl bg-white/90 backdrop-blur-sm">
              <CardHeader>
                <CardTitle className="flex items-center gap-2">
                  <Heart className="size-5 text-pink-500" />
                  Create Account
                </CardTitle>
              </CardHeader>
              <CardContent>
                <form onSubmit={handleRegister} className="space-y-4">
                  <div className="space-y-2">
                    <Label htmlFor="register-name">Name</Label>
                    <Input
                      id="register-name"
                      type="text"
                      placeholder="Your name"
                      value={registerName}
                      onChange={(e) => setRegisterName(e.target.value)}
                      required
                      className="border-purple-200 focus:border-pink-400"
                    />
                  </div>
                  <div className="space-y-2">
                    <Label htmlFor="register-username">Username</Label>
                    <Input
                      id="register-username"
                      type="text"
                      placeholder="pet_lover"
                      value={registerUsername}
                      onChange={(e) => setRegisterUsername(e.target.value)}
                      required
                      className="border-purple-200 focus:border-pink-400"
                    />
                  </div>
                  <div className="space-y-2">
                    <Label htmlFor="register-password">Password</Label>
                    <Input
                      id="register-password"
                      type="password"
                      placeholder="••••••••"
                      value={registerPassword}
                      onChange={(e) => setRegisterPassword(e.target.value)}
                      required
                      className="border-purple-200 focus:border-pink-400"
                    />
                  </div>
                  <Button
                    type="submit"
                    className="w-full bg-gradient-to-r from-pink-400 to-purple-500 hover:from-pink-500 hover:to-purple-600 text-white"
                  >
                    <Heart className="size-4 mr-2" />
                    Register
                  </Button>
                </form>
              </CardContent>
            </Card>
          </TabsContent>
        </Tabs>
      </div>
    </div>
  );
}
